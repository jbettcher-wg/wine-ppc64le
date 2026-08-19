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
 * Deliberately NOT declared, so they stay refused:
 *   _CxxThrowException, __CxxFrameHandler3 -- the EH personality belongs to
 *     the guest; a native throw/unwind over guest frames is never right.
 *   __unDNameEx -- takes caller-supplied malloc/free FUNCTION POINTERS, which
 *     from a guest are guest code; its only known wanter is the game-shipped
 *     dbghelp.dll that the builtin thunk serves anyway.
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
