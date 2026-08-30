/*
 * Flat declarations for the ucrtbase C++ ABI helpers that ARE flat.
 *
 * This header is read only by spec2thunk's signature oracle (PROBE-EXTRA in
 * ucrtbase.thunks); it is not compiled into any module.  Wine implements all
 * of these in dlls/msvcrt (cpp.c, except.c) and exports them from
 * ucrtbase.spec, but declares them in no header the oracle reads, so they
 * were refused as "no declaration found" and Cyberpunk 2077's icuuc/icuin --
 * reaching them through the staged vcruntime140's forwards -- got sentinels.
 *
 * Every declaration here is DATA-ONLY on its success path: RTTI walks read
 * the guest object's own vtable and locator structures out of the shared
 * address space (image-relative offsets are resolved against the module
 * RtlPcToFileHeader finds, which sees guest images), the __std_exception pair
 * copy/free a message through the CRT heap both sides share, and the
 * __current_exception pair hand back a stable per-thread slot.  The failure
 * paths of __RTtypeid/__RTDynamicCast throw a NATIVE C++ exception (bad_typeid
 * / bad_cast), which cannot unwind a guest frame -- a guest feeding them a
 * null or non-polymorphic object will die loudly rather than get the guest
 * exception MSVC promises.  That is accepted and this comment is the record.
 *
 * Deliberately NOT declared here, so the oracle never turns them into traps:
 *   _CxxThrowException -- the EH personality belongs to the guest; a native
 *     throw over guest frames is never right.  It is no longer a plain
 *     refused hole, though: ucrtbase.thunks now carries
 *     `FORWARD _CxxThrowException guestcrt._CxxThrowException`, which
 *     bypasses this header (and the oracle) entirely and resolves to real
 *     x86-64 code in dlls/guestcrt/cxxthrow.c.
 *   __CxxFrameHandler3 (and __CxxFrameHandler/__CxxFrameHandler2, which this
 *     module's own .spec forwards to it) -- same reasoning as
 *     _CxxThrowException above, and no longer a plain hole either: MEASURED
 *     2026-08-30, no free FH3 exists in any staged or shippable builtin
 *     (winedump -j export on every module actually staged into a Quake II
 *     prefix -- vcruntime140, vcruntime140_1, msvcp140 -- shows none of
 *     them export it, and msvcrt.thunks/msvcr100.thunks/msvcr120.thunks
 *     each separately confirm this port has never built any real msvcrt/CRT
 *     family module as a guest PE).  `dlls/guestcrt/cxxhandler3.c` now
 *     provides real x86-64 personality code (the FuncInfo-table decoder,
 *     ported from dlls/msvcrt/except.c with the scope cuts its own banner
 *     names), and ucrtbase.thunks carries `FORWARD __CxxFrameHandler3
 *     guestcrt.__CxxFrameHandler3` (and the /2 and plain names) the same
 *     way it already does for _CxxThrowException.
 *   __unDNameEx -- takes caller-supplied malloc/free FUNCTION POINTERS, which
 *     from a guest are guest code; its only known wanter is the game-shipped
 *     dbghelp.dll that the builtin thunk serves anyway.
 *
 * __processing_throw is NOT declared below, unlike the rest of this file's
 * subject -- it does not need to be.  dlls/msvcrt/msvcrt.h already declares
 * it (used internally by handler4.c/except.c), and this module's own
 * ucrtbase.thunks already carries `PROBE-EXTRA msvcrt.h` from an earlier
 * triage, so the oracle already sees it there; ucrtbase.thunks' 2026-08-29
 * guest-cxx-eh-plan Session A row cites msvcrt.h directly instead of adding
 * a second, redundant declaration here (spec2thunk's oracle hard-fails a
 * `spec disagrees with Wine's headers` mismatch if a name resolves to a
 * DIFFERENT declaration site than the one asserted -- measured, not
 * theoretical).  It hands back a POINTER into native thread-local data
 * (msvcrt_get_thread_data()->processing_throw) exactly the way
 * __current_exception does above, so guestcrt's future FH3/FH4 personality
 * (guest-cxx-eh-plan.md Session B) can read and write it directly, with no
 * host->guest call needed.  Adding it now, alongside
 * _CreateFrameInfo/_FindAndUnlinkFrame/_IsExceptionObjectToBeDestroyed also
 * in msvcrt.h, serves the BUILTIN ucrtbase/vcruntime140_1's own real FH4
 * code -- not, as an earlier draft of this comment claimed, "a staged Proton
 * vcruntime140/vcruntime140_1".  CORRECTED, 2026-08-29 (same day, adversarial
 * review): dlls/ntdll/loader.c's guest module resolution tries the builtin
 * thunk before the ordinary search path a prefix-staged file would be found
 * on, so a same-named builtin always wins and a staged file is never even
 * opened for an unqualified import -- there is no "staged" lane distinct
 * from the builtin one to serve.  The three ExceptionObject names in that
 * same section were ALSO claimed to "stay sentinels for now" -- also false
 * when written: msvcrt.h declares all three and FROM-SPEC auto emitted
 * ordinary native trap stubs for them in every module that sees that header
 * (MEASURED at RVA 0x1c0c0/0x1c0d0/0x1c0e0 in ucrtbase.dll alone), the same
 * "silent wrong answer" class this header's own banner exists to prevent,
 * since __DestructExceptionObject calls the thrown object's own destructor
 * -- a guest function pointer -- from what would have been native code.  A
 * same-day follow-up added an explicit EXCLUDE line for each name in
 * ucrtbase.thunks (and the four sibling CRT modules' .thunks files), so they
 * are true named refusals now, not an accident that happened to go unused.
 */

#ifndef __WINE_UCRTBASE_THUNKCXX_H
#define __WINE_UCRTBASE_THUNKCXX_H

#include <corecrt.h>

void       *__cdecl __RTtypeid(void *obj);
void       *__cdecl __RTDynamicCast(void *obj, int unknown, void *src_ti,
                                    void *dst_ti, int do_throw);
void       *__cdecl __RTCastToVoid(void *obj);
void      **__cdecl __current_exception(void);
void      **__cdecl __current_exception_context(void);
void        __cdecl __std_exception_copy(const void *src, void *dst);
void        __cdecl __std_exception_destroy(void *data);
int         __cdecl __std_type_info_compare(const void *lhs, const void *rhs);
void        __cdecl __std_type_info_destroy_list(void *header);
size_t      __cdecl __std_type_info_hash(const void *ti);
const char *__cdecl __std_type_info_name(void *ti, void *header);

#endif  /* __WINE_UCRTBASE_THUNKCXX_H */
