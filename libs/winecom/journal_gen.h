/*
 * journal_gen.h -- the TABLE-DRIVEN guest-side call journal: one x86-64
 * snippet generator for every void-returning COM slot whose arguments are
 * by-value scalars, proxy pointers, pointers to fixed-size structs, or
 * counted arrays of those.
 *
 * The first journal (winecom.c, install_journal) hand-encoded one snippet
 * per SHAPE for eight D3D12 command-list slots.  A D3D11 immediate context
 * has sixty-odd such slots in a dozen shapes, so the shapes become data:
 * a jg_def names the slot and describes each argument after `this` as
 *
 *     JG_V  by value (scalar, enum, handle, proxy pointer): the raw
 *           register or stack slot is stored in the record and the drain
 *           hands it to the classifier exactly as a trap would have;
 *     JG_P  pointer to `elem` bytes (a D3D11_BOX, a float[4]): the bytes
 *           are copied INTO the record, the pointer kept for its NULLness;
 *     JG_A  pointer to an array of `elem`-byte elements whose count is
 *           argument `count_arg` (1-based, after `this`), at most `max`
 *           elements: copied into the record the same way, and a count
 *           above `max` falls back to the trap BEFORE anything is written.
 *
 * Record layout (all little-endian, 8-byte aligned):
 *
 *     +0   u32  key      (iface << 16) | slot
 *     +4   u32  size | (shape << 24)     shape == JSH_GEN for these
 *     +8   u64  args[argc-1]             argument i at +8*i
 *     +..  blobs, one per JG_P/JG_A argument, at jg_layout::off[i-1]
 *
 * The record size is FIXED per slot (every array reserves `max` elements)
 * so the drain can validate a record by its size alone, like the first
 * journal does.
 *
 * THIS HEADER HAS NO WINE DEPENDENCIES ON PURPOSE.  It is included by
 * winecom.c AND by ppc64le/winecom/probes/journal_gen_host.c, a plain
 * x86-64 Linux program that generates every snippet in the table, RUNS it
 * against a fake proxy and ring, and checks the record byte by byte -- the
 * only way to test x86 machine code this port emits without a guest in
 * the loop.  Keep it that way: stdint, no TRACE, no NTSTATUS.
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __WINECOM_JOURNAL_GEN_H
#define __WINECOM_JOURNAL_GEN_H

#include <stdint.h>
#include <string.h>

enum jg_kind { JG_V = 0, JG_P = 1, JG_A = 2 };

/* the per-proxy ring fields the snippet reads, as offsets into struct
 * com_proxy -- pinned there by C_ASSERT, repeated here so the host test
 * can lay out a fake proxy the same way */
#define JG_PROXY_RING_BASE 0x28
#define JG_PROXY_RING_POS  0x30
#define JG_PROXY_RING_CAP  0x38

#define JG_MAX_ARGS 8

struct jg_arg
{
    uint8_t kind;        /* enum jg_kind */
    uint8_t count_arg;   /* JG_A: 1-based index of the count argument */
    uint8_t elem;        /* JG_P: byte size (multiple of 8, <= 248);
                            JG_A: element size, 4, 8, 16 or 24 */
    uint8_t max;         /* JG_A: maximum element count served */
};

/* scope: how the ring is drained.  JG_SCOPE_LIST is the first journal's
 * rule (one recorder per object, drained at that object's own trap);
 * JG_SCOPE_CTX registers the ring for the drain that runs at EVERY
 * dispatch, because an immediate context's recorded state is observable
 * through any other object of the surface. */
enum jg_scope { JG_SCOPE_LIST = 1, JG_SCOPE_CTX = 2 };

struct jg_def
{
    const char *name;
    uint8_t argc;        /* including `this`, must equal the table row's */
    uint8_t scope;       /* enum jg_scope */
    struct jg_arg args[JG_MAX_ARGS];
};

struct jg_layout
{
    uint32_t rec;                 /* record bytes */
    uint32_t off[JG_MAX_ARGS];    /* blob offset for argument i+1, or 0 */
};

#define JG_ROUND8(n) (((n) + 7u) & ~7u)

static inline void jg_layout_compute( const struct jg_def *def, struct jg_layout *lay )
{
    unsigned i, off = 8 + 8 * (def->argc - 1);

    memset( lay, 0, sizeof(*lay) );
    for (i = 0; i + 1 < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i];
        if (a->kind == JG_P)
        {
            lay->off[i] = off;
            off += JG_ROUND8( a->elem );
        }
        else if (a->kind == JG_A)
        {
            lay->off[i] = off;
            off += JG_ROUND8( (unsigned)a->elem * a->max );
        }
    }
    lay->rec = off;
}

/* ------------------------------------------------------------ the encoder
 *
 * Registers by their hardware numbers.  Every memory operand is emitted in
 * the one general form -- mod=10 + SIB + disp32 -- so there is exactly one
 * encoding path to get right; the bytes are a little longer than a
 * hand-picked form and nobody reads them.
 */
enum { R_RAX = 0, R_RCX = 1, R_RDX = 2, R_RSP = 4, R_R8 = 8, R_R9 = 9,
       R_R10 = 10, R_R11 = 11, R_NONE = -1 };

struct jg_buf
{
    uint8_t *p;
    uint8_t *start;
    /* rel32 sites that must land on the fallback */
    uint8_t *fb_fix[32];
    unsigned n_fb;
};

static inline void jg_e8( struct jg_buf *b, unsigned v )  { *b->p++ = (uint8_t)v; }
static inline void jg_e32( struct jg_buf *b, uint32_t v ) { memcpy( b->p, &v, 4 ); b->p += 4; }
static inline void jg_e64( struct jg_buf *b, uint64_t v ) { memcpy( b->p, &v, 8 ); b->p += 8; }

/* REX + opcode + modrm(mod=10, reg, rm=SIB) + SIB + disp32.  `reg` is the
 * register field (a register operand, or an opcode extension /n); `w` asks
 * for REX.W; `op2` is a second opcode byte when non-negative (0F xx). */
static inline void jg_mem( struct jg_buf *b, int op, int op2, int reg, int base,
                           int index, int scale, int32_t disp, int w )
{
    unsigned rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 |
                   (index >= 0 ? ((index >> 3) & 1) << 1 : 0) | ((base >> 3) & 1);
    unsigned ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;

    if (rex != 0x40) jg_e8( b, rex );
    jg_e8( b, op );
    if (op2 >= 0) jg_e8( b, op2 );
    jg_e8( b, 0x80 | ((reg & 7) << 3) | 4 );                 /* mod=10 rm=100 */
    jg_e8( b, (ss << 6) | ((index >= 0 ? index & 7 : 4) << 3) | (base & 7) );
    jg_e32( b, (uint32_t)disp );
}

/* register-register forms: REX + op + modrm(mod=11) */
static inline void jg_rr( struct jg_buf *b, int op, int reg, int rm, int w )
{
    unsigned rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1);
    if (rex != 0x40) jg_e8( b, rex );
    jg_e8( b, op );
    jg_e8( b, 0xc0 | ((reg & 7) << 3) | (rm & 7) );
}

static inline void jg_mov_load64( struct jg_buf *b, int reg, int base, int32_t disp )
{ jg_mem( b, 0x8b, -1, reg, base, R_NONE, 0, disp, 1 ); }
static inline void jg_mov_load32( struct jg_buf *b, int reg, int base, int32_t disp )
{ jg_mem( b, 0x8b, -1, reg, base, R_NONE, 0, disp, 0 ); }
static inline void jg_mov_store64( struct jg_buf *b, int base, int32_t disp, int reg )
{ jg_mem( b, 0x89, -1, reg, base, R_NONE, 0, disp, 1 ); }
static inline void jg_mov_load64_idx( struct jg_buf *b, int reg, int base, int index, int scale, int32_t disp )
{ jg_mem( b, 0x8b, -1, reg, base, index, scale, disp, 1 ); }
static inline void jg_mov_load32_idx( struct jg_buf *b, int reg, int base, int index, int scale, int32_t disp )
{ jg_mem( b, 0x8b, -1, reg, base, index, scale, disp, 0 ); }
static inline void jg_mov_store64_idx( struct jg_buf *b, int base, int index, int scale, int32_t disp, int reg )
{ jg_mem( b, 0x89, -1, reg, base, index, scale, disp, 1 ); }
static inline void jg_mov_store32_idx( struct jg_buf *b, int base, int index, int scale, int32_t disp, int reg )
{ jg_mem( b, 0x89, -1, reg, base, index, scale, disp, 0 ); }
static inline void jg_lea64( struct jg_buf *b, int reg, int base, int32_t disp )
{ jg_mem( b, 0x8d, -1, reg, base, R_NONE, 0, disp, 1 ); }
static inline void jg_cmp64_rm( struct jg_buf *b, int reg, int base, int32_t disp )
{ jg_mem( b, 0x3b, -1, reg, base, R_NONE, 0, disp, 1 ); }
static inline void jg_mov_store_imm32( struct jg_buf *b, int base, int32_t disp, uint32_t imm )
{ jg_mem( b, 0xc7, -1, 0, base, R_NONE, 0, disp, 0 ); jg_e32( b, imm ); }
static inline void jg_cmp32_imm( struct jg_buf *b, int reg, uint32_t imm )
{ jg_rr( b, 0x81, 7, reg, 0 ); jg_e32( b, imm ); }
static inline void jg_test64( struct jg_buf *b, int a, int c ) { jg_rr( b, 0x85, a, c, 1 ); }
static inline void jg_test32( struct jg_buf *b, int a, int c ) { jg_rr( b, 0x85, a, c, 0 ); }
static inline void jg_add64( struct jg_buf *b, int dst, int src ) { jg_rr( b, 0x01, src, dst, 1 ); }
static inline void jg_xor32( struct jg_buf *b, int reg ) { jg_rr( b, 0x31, reg, reg, 0 ); }
static inline void jg_inc32( struct jg_buf *b, int reg ) { jg_rr( b, 0xff, 0, reg, 0 ); }
static inline void jg_cmp32_rr( struct jg_buf *b, int a, int c ) { jg_rr( b, 0x39, c, a, 0 ); }  /* cmp a, c */
static inline void jg_imul32_imm8( struct jg_buf *b, int reg, unsigned imm8 )
{ jg_rr( b, 0x6b, reg, reg, 0 ); jg_e8( b, imm8 ); }
static inline void jg_ret( struct jg_buf *b ) { jg_e8( b, 0xc3 ); }

/* jcc rel32 to the fallback: 0F 8x + rel32, patched at the end */
static inline void jg_jcc_fb( struct jg_buf *b, unsigned cc )
{
    jg_e8( b, 0x0f ); jg_e8( b, 0x80 | cc );
    b->fb_fix[b->n_fb++] = b->p;
    jg_e32( b, 0 );
}
/* jcc rel32 forward to a label the caller patches */
static inline uint8_t *jg_jcc_fwd( struct jg_buf *b, unsigned cc )
{
    uint8_t *site;
    jg_e8( b, 0x0f ); jg_e8( b, 0x80 | cc );
    site = b->p;
    jg_e32( b, 0 );
    return site;
}
static inline void jg_patch_fwd( struct jg_buf *b, uint8_t *site )
{
    int32_t d = (int32_t)(b->p - (site + 4));
    memcpy( site, &d, 4 );
}
/* jcc rel8 backwards to `target` */
static inline void jg_jcc_back8( struct jg_buf *b, unsigned cc, const uint8_t *target )
{
    int d = (int)(target - (b->p + 2));
    jg_e8( b, 0x70 | cc );
    jg_e8( b, (uint8_t)d );
}

#define CC_B  0x2   /* below (unsigned <) */
#define CC_AE 0x3   /* above or equal */
#define CC_E  0x4   /* equal / zero */
#define CC_A  0x7   /* above (unsigned >) */

/* the MS-x64 home of argument i (1-based after `this`): a register for
 * 1..3, else the caller's stack slot [rsp + 8 + 8*i] on entry (return
 * address at [rsp], the 32-byte home area above it) */
static inline int jg_arg_reg( unsigned i )
{
    return i == 1 ? R_RDX : i == 2 ? R_R8 : i == 3 ? R_R9 : R_NONE;
}
static inline int32_t jg_arg_stack_disp( unsigned i ) { return 8 + 8 * (int32_t)i; }

/* Emit one snippet for `def` into buf.  key/shape are the record header;
 * `fallback` is the slot's original trap stub, reached by an indirect jmp
 * with EVERY argument register untouched -- nothing before a fallback
 * branch may write rcx, rdx, r8, r9 or the stack.  Returns bytes emitted.
 *
 * Register plan:  rax = record pointer (ring base, then base + pos);
 * r10 = pos, free once added into rax, then scratch; r11 = the new pos,
 * live to the epilogue; rdx/r8/r9 are stored first and then free (they
 * are volatile in MS-x64 and the snippet returns straight to the caller).
 */
static inline unsigned jg_emit( uint8_t *buf, const struct jg_def *def, const struct jg_layout *lay,
                                uint32_t key, unsigned shape, uint64_t fallback )
{
    struct jg_buf b;
    unsigned i, k;

    b.p = b.start = buf;
    b.n_fb = 0;

    /* ring present?  room for one record? */
    jg_mov_load64( &b, R_RAX, R_RCX, JG_PROXY_RING_BASE );
    jg_test64( &b, R_RAX, R_RAX );
    jg_jcc_fb( &b, CC_E );
    jg_mov_load64( &b, R_R10, R_RCX, JG_PROXY_RING_POS );
    jg_lea64( &b, R_R11, R_R10, (int32_t)lay->rec );
    jg_cmp64_rm( &b, R_R11, R_RCX, JG_PROXY_RING_CAP );
    jg_jcc_fb( &b, CC_A );
    jg_add64( &b, R_RAX, R_R10 );                 /* rax = record; r10 free */

    /* array count guards, before any store: a count above max means the
     * trap serves this call with its registers exactly as they arrived */
    for (i = 1; i < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i - 1];
        int creg;
        if (a->kind != JG_A) continue;
        creg = jg_arg_reg( a->count_arg );
        if (creg == R_NONE)
        {
            jg_mov_load32( &b, R_R10, R_RSP, jg_arg_stack_disp( a->count_arg ) );
            creg = R_R10;
        }
        jg_cmp32_imm( &b, creg, a->max );
        jg_jcc_fb( &b, CC_A );
    }

    /* header */
    jg_mov_store_imm32( &b, R_RAX, 0, key );
    jg_mov_store_imm32( &b, R_RAX, 4, lay->rec | ((uint32_t)shape << 24) );

    /* every argument as it arrived */
    for (i = 1; i < def->argc; i++)
    {
        int reg = jg_arg_reg( i );
        if (reg == R_NONE)
        {
            jg_mov_load64( &b, R_R10, R_RSP, jg_arg_stack_disp( i ) );
            reg = R_R10;
        }
        jg_mov_store64( &b, R_RAX, 8 * (int32_t)i, reg );
    }

    /* blobs: the pointer is re-read from the record (uniform for register
     * and stack arguments), NULL copies nothing */
    for (i = 1; i < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i - 1];
        uint8_t *skip;
        int32_t off = (int32_t)lay->off[i - 1];

        if (a->kind == JG_V) continue;
        jg_mov_load64( &b, R_R8, R_RAX, 8 * (int32_t)i );
        jg_test64( &b, R_R8, R_R8 );
        skip = jg_jcc_fwd( &b, CC_E );
        if (a->kind == JG_P)
        {
            for (k = 0; k < a->elem; k += 8)
            {
                jg_mov_load64( &b, R_R9, R_R8, (int32_t)k );
                jg_mov_store64( &b, R_RAX, off + (int32_t)k, R_R9 );
            }
        }
        else
        {
            uint8_t *loop;
            int scale = a->elem == 4 ? 4 : 8;
            uint8_t *skip2;

            jg_mov_load32( &b, R_RDX, R_RAX, 8 * (int32_t)a->count_arg );
            jg_test32( &b, R_RDX, R_RDX );
            skip2 = jg_jcc_fwd( &b, CC_E );
            if (a->elem > 8) jg_imul32_imm8( &b, R_RDX, a->elem / 8 );
            jg_xor32( &b, R_R10 );
            loop = b.p;
            if (scale == 4)
            {
                jg_mov_load32_idx( &b, R_R9, R_R8, R_R10, 4, 0 );
                jg_mov_store32_idx( &b, R_RAX, R_R10, 4, off, R_R9 );
            }
            else
            {
                jg_mov_load64_idx( &b, R_R9, R_R8, R_R10, 8, 0 );
                jg_mov_store64_idx( &b, R_RAX, R_R10, 8, off, R_R9 );
            }
            jg_inc32( &b, R_R10 );
            jg_cmp32_rr( &b, R_R10, R_RDX );
            jg_jcc_back8( &b, CC_B, loop );
            jg_patch_fwd( &b, skip2 );
        }
        jg_patch_fwd( &b, skip );
    }

    /* commit: publish the new position, return to the caller as a void
     * method would */
    jg_mov_store64( &b, R_RCX, JG_PROXY_RING_POS, R_R11 );
    jg_ret( &b );

    /* the fallback: every fb branch lands here; jmp [rip+0] through the
     * original stub address stored right after it */
    for (k = 0; k < b.n_fb; k++)
    {
        int32_t d = (int32_t)(b.p - (b.fb_fix[k] + 4));
        memcpy( b.fb_fix[k], &d, 4 );
    }
    jg_e8( &b, 0xff ); jg_e8( &b, 0x25 ); jg_e32( &b, 0 );
    jg_e64( &b, fallback );

    return (unsigned)(b.p - b.start);
}

/* a generous upper bound on one snippet, for the allocation */
#define JG_SNIPPET_MAX 1024

/* ------------------------------------------------------- the D3D11 table
 *
 * Curated by name from the D3D11 API, every row a VOID method of the
 * device context whose arguments are scalars, proxies, or data the
 * snippet can copy whole.  What is deliberately NOT here, and why:
 *
 *   UpdateSubresource[1]   the payload size is the resource's, unbounded
 *   ExecuteCommandList     rare, and the list proxy crossing as an argument
 *                          is the case wc_forward_host guards; keep it live
 *   Flush, Flush1          synchronization points; keep them live
 *   ClearDepthStencilView  a float in XMM2 -- served by a hand function
 *   SetResourceMinLOD      same, XMM
 *   SwapDeviceContextState has an out-parameter
 *   Map, GetData, *Get*    return values or out-parameters
 *
 * The array caps are the D3D11 binding limits where those are small
 * (8 render targets, 8 UAVs, 4 SO targets) and a working-set number where
 * the API limit is large (16 of 128 SRV slots, 16 viewports); a call above
 * the cap falls back to the trap, which is the old world.
 */
#define JGV            { JG_V, 0, 0, 0 }
#define JGP(bytes)     { JG_P, 0, (bytes), 0 }
#define JGA(c, e, m)   { JG_A, (c), (e), (m) }

static const struct jg_def jg_d3d11_defs[] =
{
    /* input assembler */
    { "ID3D11DeviceContext::IASetInputLayout",       2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::IASetPrimitiveTopology", 2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::IASetIndexBuffer",       4, JG_SCOPE_CTX, { JGV, JGV, JGV } },
    { "ID3D11DeviceContext::IASetVertexBuffers",     6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 8), JGA(2, 4, 8), JGA(2, 4, 8) } },
    /* shaders: (shader, ppClassInstances, NumClassInstances) */
    { "ID3D11DeviceContext::VSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    { "ID3D11DeviceContext::PSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    { "ID3D11DeviceContext::GSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    { "ID3D11DeviceContext::HSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    { "ID3D11DeviceContext::DSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    { "ID3D11DeviceContext::CSSetShader", 4, JG_SCOPE_CTX, { JGV, JGA(3, 8, 4), JGV } },
    /* per-stage binds: (StartSlot, Num, pp) */
    { "ID3D11DeviceContext::VSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::PSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::GSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::HSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::DSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::CSSetConstantBuffers",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::VSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::PSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::GSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::HSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::DSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::CSSetShaderResources",  4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::VSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::PSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::GSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::HSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::DSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::CSSetSamplers",         4, JG_SCOPE_CTX, { JGV, JGV, JGA(2, 8, 16) } },
    { "ID3D11DeviceContext::CSSetUnorderedAccessViews", 5, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 8), JGA(2, 4, 8) } },
    /* output merger / rasterizer / stream-out */
    { "ID3D11DeviceContext::OMSetRenderTargets", 4, JG_SCOPE_CTX, { JGV, JGA(1, 8, 8), JGV } },
    { "ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews", 8, JG_SCOPE_CTX,
      { JGV, JGA(1, 8, 8), JGV, JGV, JGV, JGA(5, 8, 8), JGA(5, 4, 8) } },
    { "ID3D11DeviceContext::OMSetBlendState",        4, JG_SCOPE_CTX, { JGV, JGP(16), JGV } },
    { "ID3D11DeviceContext::OMSetDepthStencilState", 3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::SOSetTargets",           4, JG_SCOPE_CTX, { JGV, JGA(1, 8, 4), JGA(1, 4, 4) } },
    { "ID3D11DeviceContext::RSSetState",             2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::RSSetViewports",         3, JG_SCOPE_CTX, { JGV, JGA(1, 24, 16) } },
    { "ID3D11DeviceContext::RSSetScissorRects",      3, JG_SCOPE_CTX, { JGV, JGA(1, 16, 16) } },
    /* draws and dispatches */
    { "ID3D11DeviceContext::Draw",                         3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::DrawIndexed",                  4, JG_SCOPE_CTX, { JGV, JGV, JGV } },
    { "ID3D11DeviceContext::DrawInstanced",                5, JG_SCOPE_CTX, { JGV, JGV, JGV, JGV } },
    { "ID3D11DeviceContext::DrawIndexedInstanced",         6, JG_SCOPE_CTX, { JGV, JGV, JGV, JGV, JGV } },
    { "ID3D11DeviceContext::DrawAuto",                     1, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::DrawIndexedInstancedIndirect", 3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::DrawInstancedIndirect",        3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::Dispatch",                     4, JG_SCOPE_CTX, { JGV, JGV, JGV } },
    { "ID3D11DeviceContext::DispatchIndirect",             3, JG_SCOPE_CTX, { JGV, JGV } },
    /* clears and copies */
    { "ID3D11DeviceContext::ClearRenderTargetView",        3, JG_SCOPE_CTX, { JGV, JGP(16) } },
    { "ID3D11DeviceContext::ClearUnorderedAccessViewUint", 3, JG_SCOPE_CTX, { JGV, JGP(16) } },
    { "ID3D11DeviceContext::ClearUnorderedAccessViewFloat",3, JG_SCOPE_CTX, { JGV, JGP(16) } },
    { "ID3D11DeviceContext::CopyResource",                 3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::CopySubresourceRegion",        9, JG_SCOPE_CTX,
      { JGV, JGV, JGV, JGV, JGV, JGV, JGV, JGP(24) } },
    { "ID3D11DeviceContext::ResolveSubresource",           6, JG_SCOPE_CTX, { JGV, JGV, JGV, JGV, JGV } },
    { "ID3D11DeviceContext::CopyStructureCount",           4, JG_SCOPE_CTX, { JGV, JGV, JGV } },
    { "ID3D11DeviceContext::GenerateMips",                 2, JG_SCOPE_CTX, { JGV } },
    /* queries, predication, mapping, state */
    { "ID3D11DeviceContext::Begin",          2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::End",            2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext::SetPredication", 3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::Unmap",          3, JG_SCOPE_CTX, { JGV, JGV } },
    { "ID3D11DeviceContext::ClearState",     1, JG_SCOPE_CTX, { JGV } },
    /* ID3D11DeviceContext1 */
    { "ID3D11DeviceContext1::DiscardResource", 2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext1::DiscardView",     2, JG_SCOPE_CTX, { JGV } },
    { "ID3D11DeviceContext1::ClearView",       5, JG_SCOPE_CTX, { JGV, JGP(16), JGA(4, 16, 16), JGV } },
    { "ID3D11DeviceContext1::VSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
    { "ID3D11DeviceContext1::PSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
    { "ID3D11DeviceContext1::GSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
    { "ID3D11DeviceContext1::HSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
    { "ID3D11DeviceContext1::DSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
    { "ID3D11DeviceContext1::CSSetConstantBuffers1", 6, JG_SCOPE_CTX,
      { JGV, JGV, JGA(2, 8, 16), JGA(2, 4, 16), JGA(2, 4, 16) } },
};

#undef JGV
#undef JGP
#undef JGA

#endif  /* __WINECOM_JOURNAL_GEN_H */
