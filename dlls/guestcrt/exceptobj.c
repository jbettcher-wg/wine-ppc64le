/*
 * Guest-side x86-64 exception-object lifecycle: the per-thread state that
 * __CxxFrameHandler4 reads and writes while it unwinds.
 *
 * WHY THIS EXISTS, AND WHY IT IS FIVE FUNCTIONS AND NOT THREE.
 *
 * MEASURED 2026-08-29, Cyberpunk 2077 (Steam) run 20:14.  With
 * _CxxThrowException served as real guest code (cxxthrow.c), the throw now
 * reaches a real __CxxFrameHandler4 -- and dies calling
 * vcruntime140.__CxxRegisterExceptionObject, sentinel 0xDEAD0005, from
 * VCRUNTIME140_1.dll+20bb.
 *
 * The FH4 doing that calling is NOT ours and does not need to be.  The
 * prefix's staged vcruntime140_1.dll is a WINE BUILTIN -- Proton's, x86-64
 * guest code -- and winedump says it exports exactly three names:
 *
 *     __CxxFrameHandler4, __NLG_Dispatch2, __NLG_Return2
 *
 * so the catch side already exists as guest code the guest CPU runs.  What
 * it lacks is the state those helpers keep.  Its import table names six
 * things from vcruntime140:
 *
 *     __CxxRegisterExceptionObject    __current_exception
 *     __CxxUnregisterExceptionObject  __processing_throw
 *     __DestructExceptionObject       memmove
 *
 * memmove is pure data movement and stays a trap.  The other five are this
 * file.  That is the whole gap between "a guest throw dies on a sentinel"
 * and "Microsoft-shaped C++ EH runs on Proton's own FH4" -- which is why
 * this is five exports and not the three the earlier plan called a trio.
 *
 * WHY ALL FIVE MUST BE ON THE SAME SIDE.  Read Wine's own implementations
 * (dlls/msvcrt/except.c:955-1065) and one fact dominates: every one of them
 * reaches msvcrt_get_thread_data() and touches THE SAME per-thread record.
 * __current_exception() returns &data->exc_record.
 * __CxxRegisterExceptionObject WRITES data->exc_record and data->ctx_record
 * and links a frame onto data->frame_info_head.
 * __CxxUnregisterExceptionObject reads them back and restores them.
 *
 * So serving the three Cxx* names as guest code while leaving
 * __current_exception and __processing_throw as native trap stubs would
 * split one logical structure across two machines: the guest functions
 * would write the guest copy, and the native stubs would hand FH4 a pointer
 * into the NATIVE msvcrt's thread data, which nobody wrote.  FH4 would then
 * read an exception record that does not describe the exception in flight.
 * That is the silent-wrong-answer class this tree refuses on principle, and
 * it is worse than a sentinel: it appears only during unwind, as corruption,
 * with no name attached.  A refusal that faults by name is strictly better
 * than a plausible wrong pointer.
 *
 * The same reasoning is why the state lives HERE and not in the native
 * msvcrt.  Three staged guest modules pull on it -- VCRUNTIME140_1.dll
 * (FH4), MSVCP140.dll (__DestructExceptionObject, __current_exception,
 * __processing_throw) and the staged vcruntime140.dll (__processing_throw)
 * -- and they must all reach one copy.  Routing every one of them through
 * guestcrt is what makes that true.
 *
 * WHY GUEST CODE IS REQUIRED AND NOT MERELY TIDY.  __DestructExceptionObject
 * calls the thrown object's own destructor, read as an image-relative RVA
 * out of the ThrowInfo the GUEST threw.  A native trap stub would have
 * native ppc64 code indirect-call an x86-64 destructor -- the exact landmine
 * dlls/vcruntime140/vcruntime140.thunks:162-164 documents and EXCLUDEs.
 * Here the call is guest code calling a guest pointer, which is just a call.
 *
 * PER-THREAD STORAGE.  Fls rather than a __declspec(thread) variable: this
 * module is loaded through PE forwarders after process start, so a static
 * TLS block is not guaranteed to be allocated for threads that already
 * exist, and Fls gives a destructor callback for free.  Proton's own
 * vcruntime140_1.dll imports FlsAlloc/FlsFree/FlsGetValue/FlsSetValue for
 * the same reason.  Initialisation is lazy and interlocked rather than in
 * DllMain, because DllMain is shared with setjmp.c, whose banner is
 * explicit that it sets nothing up.
 *
 * WHAT IS DELIBERATELY NOT HERE.
 *
 *   - The TYPE_FLAG_WINRT branch of __DestructExceptionObject (Wine's
 *     except.c:969-970 releases an IUnknown when no destructor is present).
 *     cxxthrow.c dropped the matching WinRT path on the throw side for the
 *     same reason -- no blocked title is WinRT -- and a half-WinRT pair
 *     would be worse than neither.  A WinRT object destructs as a plain one:
 *     it leaks its reference rather than doing something wrong.
 *
 *   - _CreateFrameInfo / _FindAndUnlinkFrame / _IsExceptionObjectToBeDestroyed
 *     are internal statics below, NOT exports.  Nothing in any staged guest
 *     module imports them (checked: VCRUNTIME140_1 and MSVCP140 import
 *     neither), so they have no ABI obligation here.  Note that
 *     dlls/msvcrt/msvcrt.h:149-154 justifies ucrtbase trap thunks for the
 *     first two on the grounds that they "only touch the native frame_info
 *     list rooted in thread_data_t.frame_info_head".  That reasoning is
 *     correct for a NATIVE caller and stale for a guest one: with this file
 *     in place the guest list is the one below.  Those trap thunks remain
 *     for native callers; a guest module that imported them directly would
 *     be a split of the same kind described above, and is worth an EXCLUDE
 *     the day one appears.
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

#define CXX_EXCEPTION         0xe06d7363
#define CXX_FRAME_MAGIC_VC6   0x19930520
#define CXX_FRAME_MAGIC_VC8   0x19930522
#define CXX_EXCEPTION_PARAMS  4

/* dlls/msvcrt/msvcrt.h:134-145 -- the caller allocates a cxx_frame_info on
 * its own stack and hands us the pointer, so this layout is ABI. */
typedef struct _frame_info
{
    void               *object;
    struct _frame_info *next;
} frame_info;

typedef struct
{
    frame_info        frame_info;
    EXCEPTION_RECORD *rec;
    CONTEXT          *context;
} cxx_frame_info;

/* dlls/msvcrt/cxx.h:375-381, the CXX_USE_RVA (x86-64) spelling: every offset
 * is an image-relative RVA against ExceptionInformation[3]. */
typedef struct
{
    UINT         flags;
    unsigned int destructor;
    unsigned int custom_handler;
    unsigned int type_info_table;
} cxx_exception_type;

/* The per-thread record.  These are exactly the thread_data_t fields
 * dlls/msvcrt/except.c touches through msvcrt_get_thread_data(); the
 * surrounding locale and mbc state has no business on the guest side. */
struct exc_state
{
    EXCEPTION_RECORD *exc_record;
    CONTEXT          *ctx_record;
    int               processing_throw;
    frame_info       *frame_info_head;
};

static DWORD fls_index = FLS_OUT_OF_INDEXES;

static void WINAPI free_exc_state( void *ptr )
{
    if (ptr) HeapFree( GetProcessHeap(), 0, ptr );
}

static struct exc_state *get_exc_state( void )
{
    struct exc_state *state;
    DWORD index;

    if ((index = fls_index) == FLS_OUT_OF_INDEXES)
    {
        if ((index = FlsAlloc( free_exc_state )) == FLS_OUT_OF_INDEXES) return NULL;
        if (InterlockedCompareExchange( (LONG *)&fls_index, index, FLS_OUT_OF_INDEXES )
            != (LONG)FLS_OUT_OF_INDEXES)
        {
            /* another thread won the race; keep its index and drop ours */
            FlsFree( index );
            index = fls_index;
        }
    }

    if (!(state = FlsGetValue( index )))
    {
        if (!(state = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state) ))) return NULL;
        if (!FlsSetValue( index, state ))
        {
            HeapFree( GetProcessHeap(), 0, state );
            return NULL;
        }
    }
    return state;
}

static BOOL is_cxx_exception( EXCEPTION_RECORD *rec )
{
    if (rec->ExceptionCode != CXX_EXCEPTION) return FALSE;
    if (rec->NumberParameters != CXX_EXCEPTION_PARAMS) return FALSE;
    return (rec->ExceptionInformation[0] >= CXX_FRAME_MAGIC_VC6 &&
            rec->ExceptionInformation[0] <= CXX_FRAME_MAGIC_VC8);
}

/* dlls/msvcrt/except.c:894 _CreateFrameInfo, kept internal -- see banner. */
static frame_info *create_frame_info( frame_info *fi, void *obj )
{
    struct exc_state *state = get_exc_state();

    if (!state) return fi;
    fi->next = state->frame_info_head;
    state->frame_info_head = fi;
    fi->object = obj;
    return fi;
}

/* dlls/msvcrt/except.c:909 _FindAndUnlinkFrame. */
static void find_and_unlink_frame( frame_info *fi )
{
    struct exc_state *state = get_exc_state();
    frame_info *cur;

    if (!state || !(cur = state->frame_info_head)) return;

    if (cur == fi)
    {
        state->frame_info_head = cur->next;
        return;
    }
    for (; cur->next; cur = cur->next)
    {
        if (cur->next == fi)
        {
            cur->next = cur->next->next;
            return;
        }
    }
}

/* dlls/msvcrt/except.c _IsExceptionObjectToBeDestroyed: an object still
 * named by a live frame on this thread is owned by that frame, not by us. */
static BOOL is_exception_object_to_be_destroyed( const void *obj )
{
    struct exc_state *state = get_exc_state();
    frame_info *cur;

    if (!state) return TRUE;
    for (cur = state->frame_info_head; cur; cur = cur->next)
        if (cur->object == obj) return FALSE;
    return TRUE;
}

/***********************************************************************
 *		__DestructExceptionObject
 *
 * Calls the thrown object's own destructor -- a GUEST function pointer,
 * which is the whole reason this file is guest code.
 */
void __cdecl __DestructExceptionObject( EXCEPTION_RECORD *rec )
{
    cxx_exception_type *info;
    void *object;

    if (!rec || !is_cxx_exception( rec )) return;
    if (!(info = (cxx_exception_type *)rec->ExceptionInformation[2])) return;
    object = (void *)rec->ExceptionInformation[1];

    /* Every offset in the ThrowInfo is an RVA against the image base the
     * throw recorded in ExceptionInformation[3] (cxxthrow.c computes it with
     * RtlPcToFileHeader, matching dlls/msvcrt/cxx.h:389 cxx_rva_base). */
    if (info->destructor)
    {
        void *dtor = (void *)(rec->ExceptionInformation[3] + info->destructor);
        /* On x86-64 there is no separate __thiscall: `this` is simply the
         * first integer argument, so a plain indirect call is the ABI.  Wine
         * routes this through an asm call_dtor (cppexcept.h:203) only to own
         * the frame for its NATIVE unwinder's benefit; guest code calling a
         * guest pointer needs no such wrapper. */
        ((void (__cdecl *)( void * ))dtor)( object );
    }
}

/***********************************************************************
 *		__CxxRegisterExceptionObject
 */
BOOL __cdecl __CxxRegisterExceptionObject( EXCEPTION_POINTERS *ep, cxx_frame_info *frame_info )
{
    struct exc_state *state;

    if (!ep || !ep->ExceptionRecord)
    {
        /* The "nothing in flight" spelling the unregister side keys on. */
        frame_info->rec = (void *)-1;
        frame_info->context = (void *)-1;
        return TRUE;
    }

    if (!(state = get_exc_state()))
    {
        frame_info->rec = (void *)-1;
        frame_info->context = (void *)-1;
        return TRUE;
    }

    frame_info->rec = state->exc_record;
    frame_info->context = state->ctx_record;
    state->exc_record = ep->ExceptionRecord;
    state->ctx_record = ep->ContextRecord;
    create_frame_info( &frame_info->frame_info,
                       (void *)ep->ExceptionRecord->ExceptionInformation[1] );
    return TRUE;
}

/***********************************************************************
 *		__CxxUnregisterExceptionObject
 */
void __cdecl __CxxUnregisterExceptionObject( cxx_frame_info *frame_info, BOOL in_use )
{
    struct exc_state *state;

    if (frame_info->rec == (void *)-1) return;
    if (!(state = get_exc_state())) return;

    find_and_unlink_frame( &frame_info->frame_info );

    if (state->exc_record &&
        state->exc_record->ExceptionCode == CXX_EXCEPTION && !in_use &&
        is_exception_object_to_be_destroyed( (void *)state->exc_record->ExceptionInformation[1] ))
        __DestructExceptionObject( state->exc_record );

    state->exc_record = frame_info->rec;
    state->ctx_record = frame_info->context;
}

/***********************************************************************
 *		__current_exception
 *
 * Returns a pointer INTO the per-thread record: FH4 reads
 * __current_exception()[-2] and dereferences *__current_exception(), so the
 * caller edits this storage directly and the address must be stable for the
 * life of the thread.  That is why exc_record is the first field.
 */
void ** __cdecl __current_exception( void )
{
    struct exc_state *state = get_exc_state();

    if (!state) return NULL;
    return (void **)&state->exc_record;
}

/***********************************************************************
 *		__current_exception_context
 */
void ** __cdecl __current_exception_context( void )
{
    struct exc_state *state = get_exc_state();

    if (!state) return NULL;
    return (void **)&state->ctx_record;
}

/***********************************************************************
 *		__processing_throw
 */
int * __cdecl __processing_throw( void )
{
    struct exc_state *state = get_exc_state();

    if (!state) return NULL;
    return &state->processing_throw;
}
