/*
 * journal_gen_host.c -- run the table-driven journal snippets NATIVELY on
 * an x86-64 Linux host, no Wine, no guest, no emulator.
 *
 * libs/winecom/journal_gen.h emits x86-64 machine code for every
 * jg_d3d11_defs row.  Nothing on the ppc64le build host can execute that
 * code, and the guest gate downstream can only see a wrong record through
 * the API's own answers, one slot at a time.  This program is the direct
 * test: for every def it generates the snippet into an executable page,
 * calls it with the MS-x64 convention (__attribute__((ms_abi)) -- the
 * same register and stack homes the snippet expects), and checks the
 * record byte for byte against what was passed:
 *
 *   1  a call within every cap: key, size|shape, every argument as given
 *      (stack arguments with dirty upper halves included), every JG_P blob,
 *      every JG_A blob up to its count, and pos advanced by exactly rec;
 *   2  NULL for every pointer argument: arguments stored, blobs untouched;
 *   3  every array count one past its cap: the FALLBACK stub is reached
 *      with rcx, rdx, r8, r9 and the stack arguments intact and pos
 *      unchanged;
 *   4  a ring with one byte too little room: fallback, pos unchanged;
 *   5  no ring (base NULL): fallback.
 *
 * Build and run on any x86-64 Linux box (the gate check-ctx-journal.sh
 * does so when it finds one, and skips this layer elsewhere):
 *
 *     cc -O1 -I libs/winecom -o /tmp/jgh ppc64le/winecom/probes/journal_gen_host.c && /tmp/jgh
 *
 * Exit 0 = every def passed, 1 = a mismatch (printed), 2 = could not run.
 *
 * Copyright 2026 the ppc64le port authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "journal_gen.h"

#define RING_CAP 8192
#define JSH_TEST_SHAPE 9

/* the fake proxy: only the three ring fields at their pinned offsets matter.
 * In the SHARED form `ring` points at a header (pos, cap, records at
 * JG_HDR_DATA) and the proxy's own pos/cap are ignored; the test drives
 * both forms through one accessor pair. */
struct fake_proxy
{
    uint8_t pad[JG_PROXY_RING_BASE];
    uint8_t *ring;
    uint64_t pos;
    uint64_t cap;
    uint8_t tail[64];
};
static int shared_form;
static uint8_t *ring_mem;   /* RING_CAP + JG_HDR_DATA bytes */
static uint64_t *pos_p( struct fake_proxy *px ) { return shared_form ? (uint64_t *)(ring_mem + JG_HDR_POS) : &px->pos; }
static uint64_t *cap_p( struct fake_proxy *px ) { return shared_form ? (uint64_t *)(ring_mem + JG_HDR_CAP) : &px->cap; }
static uint8_t *data_p( void ) { return shared_form ? ring_mem + JG_HDR_DATA : ring_mem; }

typedef uint64_t (__attribute__((ms_abi)) *snippet_fn)( void *self, uint64_t a1, uint64_t a2,
                                                          uint64_t a3, uint64_t a4, uint64_t a5,
                                                          uint64_t a6, uint64_t a7, uint64_t a8 );

/* the fallback stub: the snippet JUMPS here with the caller's frame, so a
 * function of the same signature sees the original arguments */
static int fb_hits;
static uint64_t fb_args[9];
static uint64_t __attribute__((ms_abi)) fallback_stub( void *self, uint64_t a1, uint64_t a2,
                                                        uint64_t a3, uint64_t a4, uint64_t a5,
                                                        uint64_t a6, uint64_t a7, uint64_t a8 )
{
    fb_hits++;
    fb_args[0] = (uint64_t)(uintptr_t)self;
    fb_args[1] = a1; fb_args[2] = a2; fb_args[3] = a3; fb_args[4] = a4;
    fb_args[5] = a5; fb_args[6] = a6; fb_args[7] = a7; fb_args[8] = a8;
    return 0xfa11bacc;
}

static int failures;
static void bad( const char *def, const char *what )
{
    printf( "FAIL %s: %s\n", def, what );
    failures++;
}

static uint64_t rnd( uint64_t *st )
{
    *st ^= *st << 13; *st ^= *st >> 7; *st ^= *st << 17;
    return *st;
}

/* build the argument set for one def.  count_arg values are set to the
 * counts requested; pointer arguments point at freshly randomized data. */
struct argset
{
    void *self;
    uint64_t a[9];
    uint8_t blob[JG_MAX_ARGS][8 * 256];
    unsigned count[JG_MAX_ARGS];   /* per JG_A arg, the count used */
};

static void build_args( const struct jg_def *def, struct argset *as, uint64_t *st,
                        int null_ptrs, int over_cap )
{
    unsigned i;

    memset( as, 0, sizeof(*as) );
    for (i = 1; i < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i - 1];
        unsigned k;
        if (a->kind == JG_V)
        {
            /* dirty upper halves on stack scalars, exactly what a caller's
             * 32-bit store leaves behind; the record must carry them raw */
            as->a[i] = rnd( st );
        }
        else
        {
            for (k = 0; k < sizeof(as->blob[i - 1]); k++) as->blob[i - 1][k] = (uint8_t)rnd( st );
            as->a[i] = null_ptrs ? 0 : (uint64_t)(uintptr_t)as->blob[i - 1];
        }
    }
    /* counts after the values: a count argument is a JG_V slot, and one
     * count can govern several arrays (IASetVertexBuffers has three), so
     * pick it once per count argument, within the smallest cap it serves */
    for (i = 1; i < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i - 1];
        unsigned cap = a->max, j, count;
        if (a->kind != JG_A) continue;
        for (j = 1; j < def->argc; j++)
            if (def->args[j - 1].kind == JG_A && def->args[j - 1].count_arg == a->count_arg &&
                def->args[j - 1].max < cap)
                cap = def->args[j - 1].max;
        count = over_cap ? cap + 1 : (cap ? 1 + (unsigned)(rnd( st ) % cap) : 0);
        for (j = 1; j < def->argc; j++)
            if (def->args[j - 1].kind == JG_A && def->args[j - 1].count_arg == a->count_arg)
                as->count[j - 1] = count;
        /* the count travels as a UINT: low 32 bits, upper half garbage on
         * the stack (a register count is zero-extended by the caller) */
        as->a[a->count_arg] = (uint64_t)count | (a->count_arg >= 4 ? (rnd( st ) << 32) : 0);
    }
}

static uint64_t call( snippet_fn fn, struct fake_proxy *px, struct argset *as )
{
    as->self = px;
    return fn( px, as->a[1], as->a[2], as->a[3], as->a[4], as->a[5], as->a[6], as->a[7], as->a[8] );
}

static void check_record( const struct jg_def *def, const struct jg_layout *lay,
                          const uint8_t *rec, const struct argset *as, uint32_t key,
                          int null_ptrs )
{
    unsigned i;
    uint32_t v;

    memcpy( &v, rec, 4 );
    if (v != key) bad( def->name, "key" );
    memcpy( &v, rec + 4, 4 );
    if (v != (lay->rec | (JSH_TEST_SHAPE << 24))) bad( def->name, "size|shape" );
    {
        uint64_t self;
        memcpy( &self, rec + lay->self, 8 );
        if (self != (uint64_t)(uintptr_t)as->self) bad( def->name, "this" );
    }
    for (i = 1; i < def->argc; i++)
    {
        uint64_t got;
        memcpy( &got, rec + 8 * i, 8 );
        if (got != as->a[i]) { bad( def->name, "argument value" ); printf( "   arg %u got %llx want %llx\n", i, (unsigned long long)got, (unsigned long long)as->a[i] ); }
    }
    for (i = 1; i < def->argc; i++)
    {
        const struct jg_arg *a = &def->args[i - 1];
        unsigned bytes;
        if (a->kind == JG_V) continue;
        bytes = a->kind == JG_P ? a->elem : a->elem * as->count[i - 1];
        if (null_ptrs)
        {
            /* the blob area must be untouched: the ring was zero-filled */
            unsigned k;
            for (k = 0; k < bytes; k++)
                if (rec[lay->off[i - 1] + k]) { bad( def->name, "blob written for a NULL pointer" ); break; }
        }
        else if (memcmp( rec + lay->off[i - 1], as->blob[i - 1], bytes ))
        {
            bad( def->name, "blob bytes" );
            printf( "   arg %u kind %u elem %u count %u off %u\n", i, a->kind, a->elem,
                    as->count[i - 1], lay->off[i - 1] );
        }
    }
}

int main( int argc, char **argv )
{
    uint8_t *code;
    uint8_t *ring;
    struct fake_proxy px;
    (void)ring;
    uint64_t st = 0x9e3779b97f4a7c15ull;
    unsigned d, longest = 0;
    const char *dump = argc > 1 ? argv[1] : NULL;

    code = mmap( NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    ring_mem = malloc( RING_CAP + JG_HDR_DATA );
    if (code == MAP_FAILED || !ring_mem) { puts( "journal_gen_host: no memory" ); return 2; }

    for (shared_form = 0; shared_form < 2; shared_form++)
    for (d = 0; d < sizeof(jg_d3d11_defs) / sizeof(jg_d3d11_defs[0]); d++)
    {
        const struct jg_def *def = &jg_d3d11_defs[d];
        struct jg_layout lay;
        uint32_t key = 0x00370000u | d;
        unsigned len, i, has_arr = 0, has_ptr = 0;
        struct argset as;
        uint64_t ret;
        snippet_fn fn = (snippet_fn)code;

        jg_layout_compute( def, &lay );
        len = jg_emit( code, def, &lay, key, JSH_TEST_SHAPE, (uint64_t)(uintptr_t)fallback_stub, shared_form );
        if (len > longest) longest = len;
        if (len > JG_SNIPPET_MAX) bad( def->name, "snippet longer than JG_SNIPPET_MAX" );
        if (dump && d == 0)
        {
            FILE *f = fopen( dump, "wb" );
            if (f) { fwrite( code, 1, len, f ); fclose( f ); }
        }
        for (i = 1; i < def->argc; i++)
        {
            if (def->args[i - 1].kind == JG_A) has_arr = 1;
            if (def->args[i - 1].kind != JG_V) has_ptr = 1;
        }

        /* 1: a normal call */
        memset( &px, 0, sizeof(px) );
        memset( ring_mem, 0, RING_CAP + JG_HDR_DATA );
        px.ring = ring_mem;
        *pos_p( &px ) = 16; *cap_p( &px ) = RING_CAP;   /* not at zero, to see the add */
        build_args( def, &as, &st, 0, 0 );
        fb_hits = 0;
        call( fn, &px, &as );
        if (fb_hits) bad( def->name, "normal call fell back" );
        else
        {
            if (*pos_p( &px ) != 16 + lay.rec) bad( def->name, "pos not advanced by rec" );
            check_record( def, &lay, data_p() + 16, &as, key, 0 );
        }

        /* 2: NULL pointers */
        if (has_ptr)
        {
            memset( data_p(), 0, RING_CAP );
            *pos_p( &px ) = 0; fb_hits = 0;
            build_args( def, &as, &st, 1, 0 );
            call( fn, &px, &as );
            if (fb_hits) bad( def->name, "NULL-pointer call fell back" );
            else
            {
                if (*pos_p( &px ) != lay.rec) bad( def->name, "pos (NULL case)" );
                check_record( def, &lay, data_p(), &as, key, 1 );
            }
        }

        /* 3: over the cap -> fallback with everything intact */
        if (has_arr)
        {
            *pos_p( &px ) = 0; fb_hits = 0;
            build_args( def, &as, &st, 0, 1 );
            ret = call( fn, &px, &as );
            if (fb_hits != 1 || ret != 0xfa11bacc) bad( def->name, "over-cap call did not fall back" );
            else
            {
                if (*pos_p( &px )) bad( def->name, "over-cap call moved pos" );
                if (fb_args[0] != (uint64_t)(uintptr_t)&px) bad( def->name, "fallback: rcx clobbered" );
                for (i = 1; i < def->argc; i++)
                    if (fb_args[i] != as.a[i]) { bad( def->name, "fallback: argument clobbered" ); break; }
            }
        }

        /* 4: one byte short of room */
        *pos_p( &px ) = RING_CAP - lay.rec + 1; fb_hits = 0;
        build_args( def, &as, &st, 0, 0 );
        ret = call( fn, &px, &as );
        if (fb_hits != 1 || *pos_p( &px ) != RING_CAP - lay.rec + 1) bad( def->name, "full ring did not fall back" );
        /* and exactly enough room records */
        *pos_p( &px ) = RING_CAP - lay.rec; fb_hits = 0;
        call( fn, &px, &as );
        if (fb_hits || *pos_p( &px ) != RING_CAP) bad( def->name, "exactly-enough room did not record" );
        /* a detached shared ring (cap 0) falls back too */
        if (shared_form)
        {
            *pos_p( &px ) = 0; *cap_p( &px ) = 0; fb_hits = 0;
            ret = call( fn, &px, &as );
            if (fb_hits != 1 || ret != 0xfa11bacc) bad( def->name, "cap-0 ring did not fall back" );
        }

        /* 5: no ring */
        px.ring = NULL; fb_hits = 0;
        ret = call( fn, &px, &as );
        if (fb_hits != 1 || ret != 0xfa11bacc) bad( def->name, "NULL ring did not fall back" );
    }

    printf( "journal_gen_host: %u defs x 2 forms, longest snippet %u bytes, %d failures\n",
            (unsigned)(sizeof(jg_d3d11_defs) / sizeof(jg_d3d11_defs[0])), longest, failures );
    return failures ? 1 : 0;
}
