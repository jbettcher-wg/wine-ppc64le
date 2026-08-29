/*
 * Guest-side x86-64 _CxxThrowException.
 *
 * WHY THIS IS REAL GUEST CODE AND NOT A THUNK.  _CxxThrowException's whole
 * job is to hand a SEH exception record to the dispatcher and have it walk
 * GUEST frames: dlls/ntdll/signal_ppc64.c's x86-64 search phase reads each
 * frame's .xdata, follows the import thunk it names through
 * follow_guest_jmp_thunk(), and calls the resolved address AS GUEST CODE in
 * a nested emulator run (call_guest_language_handler).  If _CxxThrowException
 * itself ran natively -- a trap-stub calling native ppc64 RtlRaiseException
 * -- the exception record would be raised over the HOST's native call stack.
 * The search phase would then walk native ppc64 frames, which carry no
 * .xdata at all, and no guest __CxxFrameHandler4 would ever be asked.  That
 * is exactly the "silent wrong answer" dlls/vcruntime140/vcruntime140.thunks
 * used to commit (see the FORWARD replacing the old 0x00000B01 trap line,
 * and the hygiene-fix comment there).  So RaiseException has to be called BY
 * THE GUEST, from the guest's own stack, which means this function's body
 * has to be guest code -- one indirect trap into kernel32.RaiseException
 * (already guest-thunk-servable, kernel32.thunks:91) is fine, because that
 * trap only marshals RaiseException's four scalar/pointer arguments; it does
 * not relocate the call itself onto a different machine's stack.
 *
 * THE CONTRACT, verbatim from this tree's own reference implementation
 * (dlls/msvcrt/cpp.c:902 CDECL _CxxThrowException, matching Microsoft's
 * documented x64 ABI and dlls/msvcrt/cppexcept.h:30-33 CXX_EXCEPTION_PARAMS):
 *
 *   args[0] = 0x19930520                     CXX_FRAME_MAGIC_VC6 -- always
 *                                             the VC6 spelling; every FH3/FH4
 *                                             consumer accepts VC6..VC8, and
 *                                             the throw side has no reason to
 *                                             claim a newer one.
 *   args[1] = pExceptionObject                the thrown object, or NULL for
 *                                             `throw;` (rethrow).
 *   args[2] = pThrowInfo                      a _ThrowInfo*, or NULL for
 *                                             `throw;`.
 *   args[3] = image base of pThrowInfo's module   -- x64-only (cppexcept.h's
 *                                             CXX_USE_RVA path, active for
 *                                             every non-i386 target): every
 *                                             offset inside _ThrowInfo
 *                                             (destructor, forwardCompat,
 *                                             pCatchableTypeArray) is an
 *                                             image-RELATIVE RVA, so a
 *                                             handler cannot use them without
 *                                             knowing which image they are
 *                                             relative to.  This is exactly
 *                                             dlls/msvcrt/cxx.h:389's
 *                                             cxx_rva_base(): "return
 *                                             RtlPcToFileHeader(ptr,&base)".
 *
 * WHY RtlPcToFileHeader IS TRUSTWORTHY HERE, AND CALLED AS A TRAP.  Unlike
 * RaiseException, RtlPcToFileHeader's job is a pure DATA lookup -- "which
 * loaded module's address range contains this pointer" -- against the
 * process's module list.  A guest's modules are real entries in that list
 * (dlls/ucrtbase/thunkcxx.h's banner already relies on this for __RTtypeid),
 * so the NATIVE ppc64 implementation sees them and answers correctly; there
 * is no guest-vs-host frame confusion the way there would be for
 * RaiseException.  It is servable, and IS served, by the ordinary
 * kernel32/ntdll oracle (kernel32.spec:1340 forwards it to
 * NTDLL.RtlPcToFileHeader; neither .thunks file needs an override line
 * because winnt.h/rtlsupportapi.h already declare it -- FROM-SPEC auto picks
 * it up on its own).
 *
 * WHAT IS DELIBERATELY NOT HERE.
 *
 *   - The WinRT `TYPE_FLAG_WINRT` indirection Wine's own _CxxThrowException
 *     carries (cpp.c:906-912, redirecting through a winrt_exception_info
 *     just before the object) is dropped.  Neither blocked title
 *     (Quake II's game_x64.dll, Cyberpunk's libxess*.dll -- see the guest-
 *     cxx-eh-plan study, section 3) is WinRT, and adding an untested branch
 *     to a "verify every value" module earns nothing.  A WinRT throw through
 *     this module gets the plain (non-WinRT) treatment rather than a named
 *     refusal, which is the one place this file's behaviour differs from
 *     Wine's own -- worth knowing if a future title needs it.
 *
 *   - __CxxFrameHandler3 / __CxxFrameHandler4 -- the CATCH side.  Nothing
 *     here decodes a FuncInfo table or runs a funclet.  That is guest-cxx-
 *     eh-plan.md's Session B, on purpose: this file only gets a guest throw
 *     as far as the dispatcher's search phase, which is enough to turn a
 *     wild native-stack raise into a NAMED refusal at the first guest FH4
 *     frame (ExceptionHandler_refused) instead of a wrong answer.
 *
 *   - __CxxRegisterExceptionObject / __CxxUnregisterExceptionObject /
 *     __DestructExceptionObject.  All three are also guest code by the same
 *     reasoning as this file (see thunkcxx.h and vcruntime140.thunks for the
 *     current state of each), but __DestructExceptionObject calls the
 *     thrown object's OWN destructor -- a GUEST function pointer read out of
 *     the ThrowInfo -- so it belongs with the rest of the FH3/FH4 support
 *     file (Session B), not bolted on here alone.
 *
 *     CORRECTION, 2026-08-29 (same day, adversarial review): the commit that
 *     added this file described these three as staying "sentinels" pending
 *     Session B.  That was FALSE the moment it was written.
 *     msvcrt.h:156-158 declares all three, and the ucrtbase/vcruntime140/
 *     msvcrt/msvcr100/msvcr120 thunks all PROBE-EXTRA or otherwise see that
 *     header, so FROM-SPEC auto emitted all three as ordinary native TRAP
 *     stubs in every one of those five modules, MEASURED by reading the
 *     built PEs' export tables (e.g. ucrtbase.dll RVA 0x1c0c0/0x1c0d0/
 *     0x1c0e0, vcruntime140.dll RVA 0x2030/0x2040/0x2050).  That is exactly
 *     the "silent wrong answer" class this file's own banner and
 *     thunkcxx.h's doctrine both exist to refuse:
 *     __DestructExceptionObject's trap would have native ppc64 code
 *     indirect-call a guest x86-64 destructor pointer the first moment any
 *     FH3/FH4 personality called it.  Nothing reached it before this
 *     correction (FH4 is a genuine sentinel today), so it was a
 *     mis-documented armed landmine, not yet an active regression.  A
 *     follow-up, same-day fix added an explicit EXCLUDE line for each name
 *     in all five .thunks files, so they are now true named refusals -- see
 *     ucrtbase.thunks for the full correction.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "rtlsupportapi.h"

#define CXX_FRAME_MAGIC_VC6   0x19930520
#define CXX_EXCEPTION         0xe06d7363
#define CXX_EXCEPTION_PARAMS  4          /* object, type, magic, image base -- x64 always has 4 */

void WINAPI DECLSPEC_NORETURN _CxxThrowException( void *pExceptionObject, void *pThrowInfo )
{
    ULONG_PTR args[CXX_EXCEPTION_PARAMS];
    void *base;

    args[0] = CXX_FRAME_MAGIC_VC6;
    args[1] = (ULONG_PTR)pExceptionObject;
    args[2] = (ULONG_PTR)pThrowInfo;
    /* `throw;` passes pThrowInfo == NULL; RtlPcToFileHeader(NULL,&base) finds
     * no module and returns NULL, so args[3] comes out 0 -- the same
     * "rethrow spelling" a handler keys on via args[1]==args[2]==0. */
    args[3] = (ULONG_PTR)RtlPcToFileHeader( pThrowInfo, &base );

    for (;;) RaiseException( CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE,
                              CXX_EXCEPTION_PARAMS, args );
}
