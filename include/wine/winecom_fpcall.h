/*
 * The ONE shared implementation of winecom_invoke_fp_fn (wine/winecom.h):
 * split an integer-view argument array into ELFv2's two register files and
 * call a native vtable slot that carries by-value floats.
 *
 * Two clients today -- mfplat's direct invoker (dlls/mfplat/mfcom.c) and the
 * d3d11 lane's unix side (dlls/d3d11) -- and the point of this header is
 * that there is exactly one splitter and one caller to get wrong.  The
 * splitting rule is the flat lane's, copied deliberately and not abstracted
 * from it (dlls/ntdll/signal_ppc64.c marshal_thunk_args_fp has the worked
 * measurement -- ldexp()'s exponent read from the wrong register -- and
 * call_native_thunk_fp the caller this one mirrors):
 *
 *   - ELFv2 fills FPRs by ORDER: the n'th floating-point argument travels in
 *     f(n+1) whatever its position.
 *   - GPRs are filled by POSITION, and a floating-point argument SKIPS its
 *     GPR rather than letting the next integer close up behind it.
 *   - a `float` travels in an FPR as the double-precision value, so singles
 *     are widened here; their raw bits arrive in the LOW FOUR BYTES of the
 *     argument slot, exactly as an MS-x64 stack slot or XMM register holds
 *     them.
 *   - arguments 8 and up own doublewords in the parameter save area and
 *     travel in no register at all; the caller below writes them, because
 *     unlike a C call there is no compiler to lay the frame out.
 *
 * The fpword encoding is the flat lane's (THUNK_FP_* in signal_ppc64.c):
 * bits 0..7 = parameter i counting AFTER `this` is floating point, bits
 * 8..15 = that parameter is a single, bits 16..17 = the return (0 none,
 * 1 double, 2 float).  Bit i names OVERALL position i+1; position 0,
 * `this`, is never floating point.
 *
 * Usage, once per client object file:
 *
 *     #include "wine/winecom_fpcall.h"
 *     WINECOM_DEFINE_FP_CALLER( my_fp_caller )
 *     ...
 *     ret = winecom_fp_invoke( my_fp_caller, fn, argc, args, fpword,
 *                              &fpret_bits );
 *
 * WINECOM_DEFINE_FP_CALLER emits a file-local asm function; defining it in
 * two objects of one module is a duplicate symbol, so give each client
 * module exactly one home.
 */

#ifndef __WINE_WINECOM_FPCALL_H
#define __WINE_WINECOM_FPCALL_H

#ifdef __powerpc64__

#include "wine/asm.h"

#define WINECOM_FPCALL_MAX_ARGS   16
#define WINECOM_FPCALL_MAX_FPR     8

/* The raw caller: gpr[0..15] pre-split (positions, FP slots zero),
 * fpr[0..7] pre-split (by order, widened to double), f1's bits stored to
 * *fp_ret unconditionally (the caller of the caller decides whether they
 * mean anything).  A clone of call_native_thunk_fp; the frame comment there
 * explains every slot.  16 argument doublewords: 32(1)..159(1) is the
 * callee's parameter save area, ours above it. */
#define WINECOM_DEFINE_FP_CALLER( name ) \
    extern UINT64 name( void *proc, const UINT64 *gpr, const double *fpr, \
                        double *fp_ret ); \
    __ASM_GLOBAL_FUNC( name, \
                   "addis 2, 12, .TOC.-" __ASM_NAME(#name) "@ha\n\t" \
                   "addi 2, 2, .TOC.-" __ASM_NAME(#name) "@l\n\t" \
                   ".localentry " __ASM_NAME(#name) ", .-" __ASM_NAME(#name) "\n\t" \
                   "mflr 0\n\t" \
                   "std 0, 16(1)\n\t" \
                   __ASM_CFI(".cfi_offset 65, 16\n\t") \
                   "stdu 1, -176(1)\n\t" \
                   __ASM_CFI(".cfi_def_cfa_offset 176\n\t") \
                   "std 2, 160(1)\n\t" \
                   "std 6, 168(1)\n\t"           /* fp_ret, dead across the call */ \
                   "mtctr 3\n\t" \
                   "mr 12, 3\n\t"                /* ELFv2 global entry wants r12 */ \
                   "lfd 1, 0(5)\n\t" \
                   "lfd 2, 8(5)\n\t" \
                   "lfd 3, 16(5)\n\t" \
                   "lfd 4, 24(5)\n\t" \
                   "lfd 5, 32(5)\n\t" \
                   "lfd 6, 40(5)\n\t" \
                   "lfd 7, 48(5)\n\t" \
                   "lfd 8, 56(5)\n\t" \
                   /* arguments 8..15: parameter save area doublewords */ \
                   "ld 0, 64(4)\n\t"  "std 0, 96(1)\n\t" \
                   "ld 0, 72(4)\n\t"  "std 0, 104(1)\n\t" \
                   "ld 0, 80(4)\n\t"  "std 0, 112(1)\n\t" \
                   "ld 0, 88(4)\n\t"  "std 0, 120(1)\n\t" \
                   "ld 0, 96(4)\n\t"  "std 0, 128(1)\n\t" \
                   "ld 0, 104(4)\n\t" "std 0, 136(1)\n\t" \
                   "ld 0, 112(4)\n\t" "std 0, 144(1)\n\t" \
                   "ld 0, 120(4)\n\t" "std 0, 152(1)\n\t" \
                   "ld 10, 56(4)\n\t" \
                   "ld 9, 48(4)\n\t" \
                   "ld 8, 40(4)\n\t" \
                   "ld 7, 32(4)\n\t" \
                   "ld 6, 24(4)\n\t" \
                   "ld 5, 16(4)\n\t" \
                   "ld 3, 0(4)\n\t" \
                   "ld 4, 8(4)\n\t"              /* last: r4 is the base */ \
                   "bctrl\n\t" \
                   "ld 2, 160(1)\n\t"            /* the callee may clobber r2 */ \
                   "ld 11, 168(1)\n\t" \
                   "stfd 1, 0(11)\n\t"           /* f1 holds any FP return */ \
                   "addi 1, 1, 176\n\t" \
                   "ld 0, 16(1)\n\t" \
                   "mtlr 0\n\t" \
                   "blr" )

/* Split + call + return.  args[0] is `this` (already the host); the caller
 * of this helper owns writing it.  Returns the callee's integer result
 * (RAX's worth); *fpret_bits gets f1's double-format bits whenever fpword
 * names a floating-point return -- the CALLER converts to float width,
 * because the guest-visible write (XMM0, whole register) is the
 * dispatcher's job and lives beside its integer twin. */
static inline UINT64 winecom_fp_invoke( UINT64 (*caller)( void *, const UINT64 *,
                                                          const double *, double * ),
                                        void *fn, UINT argc, const UINT64 *args,
                                        UINT fpword, UINT64 *fpret_bits )
{
    UINT64 gpr[WINECOM_FPCALL_MAX_ARGS] = { 0 };
    double fpr[WINECOM_FPCALL_MAX_FPR] = { 0 };
    union { UINT64 bits; double d; } ret;
    UINT i, nfpr = 0;
    UINT mask = fpword & 0xffu, single = (fpword >> 8) & 0xffu;

    for (i = 0; i < argc && i < WINECOM_FPCALL_MAX_ARGS; i++)
    {
        /* bit i-1 of the masks names overall position i; `this` (i == 0)
         * is never floating point */
        if (i && (mask & (1u << (i - 1))))
        {
            if (nfpr < WINECOM_FPCALL_MAX_FPR)
            {
                if (single & (1u << (i - 1)))
                {
                    union { UINT bits; float f; } v;
                    v.bits = (UINT)args[i];
                    fpr[nfpr++] = (double)v.f;
                }
                else
                {
                    union { UINT64 bits; double d; } v;
                    v.bits = args[i];
                    fpr[nfpr++] = v.d;
                }
            }
            /* gpr[i] stays zero: the FP argument's GPR slot is skipped,
             * never filled by the next integer -- the whole trap. */
            continue;
        }
        gpr[i] = args[i];
    }
    ret.bits = 0;
    {
        UINT64 rax = caller( fn, gpr, fpr, &ret.d );
        if (fpret_bits) *fpret_bits = ret.bits;
        return rax;
    }
}

#else /* __powerpc64__ */

/* The NON-ppc64 arm exists so a client module (mfcom.c and friends) compiles
 * unchanged when the SAME source is built for the other PE arch (the i386
 * builtin build reuses dlls/mfplat's sources wholesale).  Nothing here is
 * ever CALLED on that arch: the winecom runtime is the ppc64 side's, and the
 * i386 lane's own dispatch refuses fp rows (refuse32/no-FP-plumbing, fail
 * closed).  So the caller is a null stub and the invoke answers the same
 * E_NOTIMPL the runtime would -- reached only if someone wires it where it
 * cannot belong, and loud in a debugger rather than silently wrong.
 * [MEASURED] Without this arm the i386-windows build of mfcom.c does not
 * compile at all -- caught by the first clean rebuild after the FP work
 * landed; the committed tree's stale i386-windows/mfcom.o had been hiding
 * it behind the known missing makedep edge. */

#define WINECOM_DEFINE_FP_CALLER( name ) \
    static UINT64 name( void *fn, const UINT64 *gpr, const double *fpr, \
                        double *fp_ret ) \
    { (void)fn; (void)gpr; (void)fpr; (void)fp_ret; return 0x80004001u /* E_NOTIMPL */; }

static inline UINT64 winecom_fp_invoke( UINT64 (*caller)( void *, const UINT64 *,
                                                          const double *, double * ),
                                        void *fn, UINT argc, const UINT64 *args,
                                        UINT fpword, UINT64 *fpret_bits )
{
    (void)caller; (void)fn; (void)argc; (void)args; (void)fpword;
    if (fpret_bits) *fpret_bits = 0;
    return 0x80004001u; /* E_NOTIMPL */
}

#endif /* __powerpc64__ */

#endif /* __WINE_WINECOM_FPCALL_H */
