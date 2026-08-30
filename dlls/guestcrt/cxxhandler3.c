/*
 * Guest-side x86-64 __CxxFrameHandler3 -- the FuncInfo-table personality
 * VC2015-and-earlier /MD C++ code (and this port's own clang -target
 * x86_64-windows-msvc gate lane, which can only ever EMIT FH3) links against.
 *
 * MEASURED 2026-08-30, Quake II (2023 rerelease): no free FH3 exists anywhere
 * in this tree's staged or shippable guest builtins.  `winedump -j export` on
 * every module actually staged into the run prefix's sysx8664 (vcruntime140,
 * vcruntime140_1, msvcp140 -- all three confirmed "This is a Wine builtin
 * DLL", i.e. THIS port's own build, not Microsoft's) shows FH4 real
 * (vcruntime140_1 carries genuine __CxxFrameHandler4 code, per
 * exceptobj.c's banner), but none of the three exports __CxxFrameHandler3 at
 * all.  dlls/msvcrt/msvcrt.thunks, dlls/msvcr100/msvcr100.thunks and
 * dlls/msvcr120/msvcr120.thunks all separately document it as a deliberate,
 * documented hole -- this port has never built ANY module's msvcrt/CRT
 * family as a real guest PE (the set of dlls/<mod>/<mod>.guestpe files lists
 * exactly two entries, dlls/lsteamclient/steamclient64.guestpe and this
 * module, before this file)
 * -- and no genuine Microsoft CRT redistributable (msvcr71..120, which DO
 * carry real FH3 machine code) is staged into this port's own prefix layout;
 * those only exist in the unrelated Steam Proton compatdata this run never
 * reads.  So: unlike FH4, there is no "essentially for free" answer here.
 * This file is what has to be written instead.
 *
 * THE CONTRACT (dlls/msvcrt/except.c:376-508, dlls/msvcrt/cppexcept.h,
 * dlls/msvcrt/cxx.h's CXX_USE_RVA branch -- the layout active for every
 * non-i386 target, x64 included).  Reproduced here rather than included: the
 * private msvcrt.h-adjacent headers pull in msvcrt's own thread-data and
 * TRACE infrastructure, which is unavailable under the guestpe recipe's
 * -nostdlibinc, -I dirs (tools/guestpe/guestpe's fixed, hand-measured
 * command line -- see that file's own banner for why it is not extended
 * casually).  Every structure below is reproduced field-for-field from the
 * cited source, x64's CXX_USE_RVA spelling (every pointer inside FuncInfo is
 * an image-relative RVA, exactly as cxxthrow.c's ExceptionInformation[3]
 * already established for the throw side).
 *
 * WHY THIS MUST BE GUEST CODE, AND NOT MERELY TIDY.  Three separate reasons,
 * each sufficient alone:
 *   1. It decodes a FuncInfo table that is part of the THROWING IMAGE's own
 *      .rdata, addressed by image-relative RVA against that image's base --
 *      data a native ppc64 reader could technically parse, but which then
 *      names GUEST CODE ADDRESSES (catch handlers, unwind funclets) that
 *      must be CALLED.
 *   2. Those calls are ordinary x86-64 direct calls with the funclet's
 *      EstablisherFrame in RDX (the MS x64 funclet ABI) -- calling a guest
 *      function pointer from native code is the exact landmine
 *      dlls/vcruntime140/vcruntime140.thunks:162-164 already refuses by
 *      name for `_CxxThrowException`, and it is no safer here.
 *   3. RtlUnwindEx's STATUS_UNWIND_CONSOLIDATE mechanism (find_catch_block,
 *      below) calls ExceptionInformation[0] back AS GUEST CODE once the
 *      unwind reaches the catching frame (dlls/ntdll/signal_ppc64.c
 *      guest_consolidate_callback, already implemented and independently
 *      exercised by ppc64le/seh/check-seh-handlers.sh's synthetic records --
 *      see that function's own banner) -- so the consolidation routine
 *      itself, call_catch_block below, has to be guest code too, the same
 *      argument dlls/msvcrt/except.c's own call_catch_block already makes.
 *
 * SCOPE CUTS, NAMED RATHER THAN HIDDEN.
 *
 *   - RtlLookupFunctionEntry on a bare guest AMD64 pc (used by upstream's
 *     cxx_frame_handler purely to detect a NESTED exception -- one thrown
 *     from inside a catch/unwind funclet of THIS SAME function) is untested
 *     on this port for a synthetic pc (guest-cxx-eh-plan.md section 6.3) and
 *     is SIDESTEPPED per that plan's own fallback: dispatch->ControlPc, by
 *     construction, is inside dispatch->FunctionEntry's own range -- that is
 *     how the search-phase walk found this handler -- so
 *     dispatch->FunctionEntry->BeginAddress is exactly what
 *     RtlLookupFunctionEntry(dispatch->ControlPc, ...) would report, with no
 *     extra guest call and nothing new to distrust.
 *
 *   - `_set_se_translator` (translating a plain SEH exception into a C++
 *     throw) is not implemented: this file simply never installs one, which
 *     is IDENTICAL to upstream's own behaviour on a thread that never called
 *     _set_se_translator (by far the common case).  A title that DOES use
 *     it gets a plain SEH search past this frame instead of a translated
 *     C++ catch -- a narrower, not a wrong, answer: __CxxFrameHandler3
 *     still runs cxx_local_unwind and check_noexcept correctly either way.
 *
 *   - An exception thrown FROM INSIDE a catch handler that must escape the
 *     handler (not caught by a still-more-nested try within the same
 *     funclet) is a pre-existing, already-named limit of the CONSOLIDATE
 *     mechanism itself, not something this file introduces:
 *     guest_consolidate_callback's own banner states it in as many words
 *     ("a `throw` from inside the catch block is a raise inside the nested
 *     run... one that must escape the catch block does not [work]").
 *     call_catch_block below therefore does the ONE thing that is provably
 *     correct on the path that DOES return -- register before, run the
 *     funclet, unregister-and-destroy after a NORMAL return, which is the
 *     only way execution reaches that line -- and does not attempt Wine's
 *     own __TRY/__FINALLY_CTX safety net around it.  Porting that net would
 *     mean porting __wine_setjmpex/__wine_longjmp/__wine_exception_handler
 *     as GUEST code too (dlls/ntdll/signal_ppc64.c's "Wine TEB-frame hack"
 *     comment at dispatch_guest_frames() confirms the dispatcher would honour
 *     it if it existed), which is real, out-of-band work belonging to its
 *     own session, not bolted on here half-tested.
 *
 * WHAT IS SUPPORTED: single and nested try/catch, catch by value/reference,
 * catch(...), scope-exit local unwind (destructors of locals leaving a try
 * block run correctly via cxx_local_unwind), a bare `throw;` rethrow at the
 * TOP level of a handler (resolved through __current_exception(), the same
 * per-thread record __CxxRegisterExceptionObject/UnregisterExceptionObject
 * already maintain), and noexcept termination.  An unrecognised FuncInfo
 * magic or a synchronous-only frame handed a foreign exception both fall
 * through to ExceptionContinueSearch exactly as upstream does -- neither is
 * a case this file can misinterpret, so neither needs a trap.
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
#define CXX_FRAME_MAGIC_VC7   0x19930521
#define CXX_FRAME_MAGIC_VC8   0x19930522
#define CXX_EXCEPTION_PARAMS  4

#define FUNC_DESCR_SYNCHRONOUS 1
#define FUNC_DESCR_NOEXCEPT    4

#define CLASS_IS_SIMPLE_TYPE         1
#define CLASS_HAS_VIRTUAL_BASE_CLASS 4
#define CLASS_IS_WINRT               8

#define TYPE_FLAG_CONST     1
#define TYPE_FLAG_VOLATILE  2
#define TYPE_FLAG_REFERENCE 8
#define TYPE_FLAG_WINRT    16

/* ---- exceptobj.c's exports: same module, ordinary intra-image calls ---- */
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

extern BOOL __cdecl __CxxRegisterExceptionObject( EXCEPTION_POINTERS *ep, cxx_frame_info *fi );
extern void __cdecl __CxxUnregisterExceptionObject( cxx_frame_info *fi, BOOL in_use );
extern void __cdecl __DestructExceptionObject( EXCEPTION_RECORD *rec );
extern void ** __cdecl __current_exception( void );

/* ---- dlls/msvcrt/cxx.h, CXX_USE_RVA (x64) branch, verbatim layout ---- */
typedef struct __type_info
{
    const void *vtable;
    char       *name;
    char        mangled[128];
} type_info;

typedef struct
{
    int this_offset;
    int vbase_descr;
    int vbase_offset;
} this_ptr_offsets;

typedef struct
{
    UINT             flags;
    unsigned int     type_info;
    this_ptr_offsets offsets;
    unsigned int     size;
    unsigned int     copy_ctor;
} cxx_type_info;

typedef struct
{
    UINT         count;
    unsigned int info[5];
} cxx_type_info_table;

typedef struct
{
    UINT         flags;
    unsigned int destructor;
    unsigned int custom_handler;
    unsigned int type_info_table;
} cxx_exception_type;

/* ---- dlls/msvcrt/cppexcept.h, CXX_USE_RVA branch, verbatim layout ---- */
typedef struct
{
    UINT ip;
    int  state;
} ipmap_info;

typedef struct
{
    UINT flags;
    UINT type_info;
    int  offset;
    UINT handler;
    UINT frame;                 /* _WIN64 only -- always built for x86-64 here */
} catchblock_info;

typedef struct
{
    int  start_level;
    int  end_level;
    int  catch_level;
    UINT catchblock_count;
    UINT catchblock;
} tryblock_info;

typedef struct
{
    int  prev;
    UINT handler;
} unwind_info;

typedef struct
{
    UINT magic : 29;
    UINT bbt_flags : 3;
    UINT unwind_count;
    UINT unwind_table;
    UINT tryblock_count;
    UINT tryblock;
    UINT ipmap_count;
    UINT ipmap;
    int  unwind_help;
    UINT expect_list;
    UINT flags;
} cxx_function_descr;

static inline void *cxx_rva( unsigned int rva, ULONG_PTR base )
{
    return (void *)(base + rva);
}

/* Bytewise, overlap-safe copy -- built -fno-builtin, so a bare memmove call
 * would be an unresolved import for the sake of moving at most a few dozen
 * bytes (an exception object's own size).  Not worth a new IMPORT line. */
static void cxx_memmove( void *dst, const void *src, unsigned int n )
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
}

static void cxx_memzero( void *dst, unsigned int n )
{
    unsigned char *d = dst;
    while (n--) *d++ = 0;
}

/* dlls/msvcrt/except.c: get_this_pointer -- unchanged, no external state. */
static void *get_this_pointer( const this_ptr_offsets *off, void *object )
{
    if (!object) return NULL;
    if (off->vbase_descr >= 0)
    {
        int *offset_ptr;
        object = (char *)object + off->vbase_descr;
        offset_ptr = (int *)(*(char **)object + off->vbase_offset);
        object = (char *)object + *offset_ptr;
    }
    return (char *)object + off->this_offset;
}

/* dlls/msvcrt/cppexcept.h: find_caught_type, one behavioural difference --
 * upstream compares mangled names when the type_info pointer differs
 * (RTTI across module boundaries can produce equal-content, distinct
 * type_info objects); strcmp needs no import here (a tiny local one, next
 * function), so the fidelity is kept rather than cut. */
static int cxx_strcmp( const char *a, const char *b )
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static const cxx_type_info *find_caught_type( cxx_exception_type *exc_type, ULONG_PTR base,
                                              const type_info *catch_ti, UINT catch_flags )
{
    const cxx_type_info_table *table = cxx_rva( exc_type->type_info_table, base );
    UINT i;

    for (i = 0; i < table->count; i++)
    {
        const cxx_type_info *type = cxx_rva( table->info[i], base );
        const type_info *ti = cxx_rva( type->type_info, base );

        if (!catch_ti) return type;                        /* catch(...) */
        if (catch_ti != ti && cxx_strcmp( catch_ti->mangled, ti->mangled )) continue;
        if ((exc_type->flags & TYPE_FLAG_CONST) && !(catch_flags & TYPE_FLAG_CONST)) continue;
        if ((exc_type->flags & TYPE_FLAG_VOLATILE) && !(catch_flags & TYPE_FLAG_VOLATILE)) continue;
        return type;
    }
    return NULL;
}

/* dlls/msvcrt/cppexcept.h: copy_exception, WinRT branch dropped -- see
 * cxxthrow.c's banner for why (no blocked title is WinRT; a half-WinRT pair
 * would be worse than neither). */
static void copy_exception( void *object, void **dest, UINT catch_flags,
                            const cxx_type_info *type, ULONG_PTR base )
{
    if (catch_flags & TYPE_FLAG_REFERENCE)
    {
        *dest = get_this_pointer( &type->offsets, object );
    }
    else if (type->flags & CLASS_IS_SIMPLE_TYPE)
    {
        cxx_memmove( dest, object, type->size );
        if (type->size == sizeof(void *)) *dest = get_this_pointer( &type->offsets, *dest );
    }
    else if (type->copy_ctor)
    {
        void *dtor_this = get_this_pointer( &type->offsets, object );
        BOOL has_vbase = (type->flags & CLASS_HAS_VIRTUAL_BASE_CLASS) != 0;
        void *ctor = cxx_rva( type->copy_ctor, base );

        if (has_vbase)
            ((void (__cdecl *)(void *, void *, int))ctor)( dest, dtor_this, 1 );
        else
            ((void (__cdecl *)(void *, void *))ctor)( dest, dtor_this );
    }
    else
    {
        cxx_memmove( dest, get_this_pointer( &type->offsets, object ), type->size );
    }
}

/* dlls/msvcrt/except.c: find_catch_handler. */
static void *find_catch_handler( void *object, ULONG_PTR frame, ULONG_PTR exc_base,
                                 const tryblock_info *tryblock, cxx_exception_type *exc_type,
                                 ULONG_PTR image_base )
{
    const catchblock_info *catchblock = cxx_rva( tryblock->catchblock, image_base );
    UINT i;

    for (i = 0; i < tryblock->catchblock_count; i++)
    {
        if (exc_type)
        {
            const type_info *catch_ti = catchblock[i].type_info ?
                cxx_rva( catchblock[i].type_info, image_base ) : NULL;
            const cxx_type_info *type = find_caught_type( exc_type, exc_base, catch_ti,
                                                           catchblock[i].flags );
            if (!type) continue;
            if (catch_ti && catch_ti->mangled[0] && catchblock[i].offset)
            {
                void **dest = (void **)(frame + catchblock[i].offset);
                copy_exception( object, dest, catchblock[i].flags, type, exc_base );
            }
        }
        else if (catchblock[i].type_info) continue;       /* only catch(...) matches a plain SEH rec */

        return cxx_rva( catchblock[i].handler, image_base );
    }
    return NULL;
}

/* ---------------------------------------------------------------------
 * call_funclet -- the x64 funclet ABI, byte-identical to
 * dlls/msvcrt/except_x86_64.c's call_exc_handler: the funclet is entered
 * with its EstablisherFrame in RDX and nothing else defined.  A plain guest
 * call (no thunk crossing: the funclet is guest code, the caller is guest
 * code) with its own .pdata so unwinding can pass back THROUGH this frame
 * if the funclet itself does not return (see this file's banner on the
 * consolidate mechanism's own already-named limit for that path).
 * ------------------------------------------------------------------- */
extern void *call_funclet( void *handler, ULONG_PTR frame );
__asm__(
    ".text\n"
    ".globl call_funclet\n"
    ".seh_proc call_funclet\n"
    "call_funclet:\n\t"
    "subq $0x28,%rsp\n\t"
    ".seh_stackalloc 0x28\n\t"
    ".seh_endprologue\n\t"
    "callq *%rcx\n\t"            /* RDX (frame) untouched: the funclet's arg */
    "addq $0x28,%rsp\n\t"
    "retq\n"
    ".seh_endproc\n"
);

static int ip_to_state( const cxx_function_descr *descr, ULONG_PTR ip, ULONG_PTR base )
{
    const ipmap_info *ipmap = cxx_rva( descr->ipmap, base );
    UINT i;

    for (i = 0; i < descr->ipmap_count; i++) if (base + ipmap[i].ip > ip) break;
    return i ? ipmap[i - 1].state : -1;
}

static void cxx_local_unwind( ULONG_PTR frame, DISPATCHER_CONTEXT *dispatch,
                              const cxx_function_descr *descr, int last_level )
{
    const unwind_info *unwind_table = cxx_rva( descr->unwind_table, dispatch->ImageBase );
    int *unwind_help = (int *)(frame + descr->unwind_help);
    int trylevel = unwind_help[0];

    if (trylevel == -2) trylevel = ip_to_state( descr, dispatch->ControlPc, dispatch->ImageBase );

    while (trylevel > last_level)
    {
        if (trylevel < 0 || (UINT)trylevel >= descr->unwind_count)
        {
            /* A trylevel outside the table is not a value this file's own
             * bookkeeping can produce; the FuncInfo it was handed is
             * inconsistent with the frame it describes.  Named, not
             * silent -- ud2 at a symbol this module owns, the same idiom
             * setjmp.c's longjmp uses for its own unrepresentable case. */
            __builtin_trap();
        }
        if (unwind_table[trylevel].handler)
            call_funclet( cxx_rva( unwind_table[trylevel].handler, dispatch->ImageBase ), frame );
        trylevel = unwind_table[trylevel].prev;
    }
    unwind_help[0] = trylevel;
}

/* dlls/msvcrt/except.c: call_catch_block / __CxxCallCatchBlock's own
 * protocol -- see this file's banner for the escaping-exception limit this
 * inherits from guest_consolidate_callback rather than re-solves. */
static void * __stdcall call_catch_block( EXCEPTION_RECORD *rec )
{
    ULONG_PTR frame = rec->ExceptionInformation[1];
    const cxx_function_descr *descr = (void *)rec->ExceptionInformation[2];
    EXCEPTION_RECORD *prev_rec = (void *)rec->ExceptionInformation[6];
    CONTEXT *context = (void *)rec->ExceptionInformation[7];
    void *handler = (void *)rec->ExceptionInformation[5];
    int *unwind_help = (int *)(frame + descr->unwind_help);
    EXCEPTION_POINTERS ep = { prev_rec, context };
    cxx_frame_info fi;
    void *ret_addr;

    __CxxRegisterExceptionObject( &ep, &fi );
    ret_addr = call_funclet( handler, frame );
    /* Reached only on a NORMAL return from the funclet -- anything the
     * funclet raised and did not itself catch ends the nested run instead
     * of returning here (this file's banner), so "not in use" is not a
     * guess: it is the one thing a normal return can mean. */
    __CxxUnregisterExceptionObject( &fi, FALSE );

    unwind_help[0] = -2;
    unwind_help[1] = -1;
    return ret_addr;
}

/* dlls/msvcrt/except.c: find_catch_block. */
static void find_catch_block( EXCEPTION_RECORD *rec, CONTEXT *context, ULONG_PTR frame,
                              DISPATCHER_CONTEXT *dispatch, const cxx_function_descr *descr,
                              cxx_exception_type *info, ULONG_PTR orig_frame )
{
    ULONG_PTR exc_base = (rec->NumberParameters == 4 ? rec->ExceptionInformation[3] : 0);
    void *object = (void *)rec->ExceptionInformation[1];
    int trylevel = ip_to_state( descr, dispatch->ControlPc, dispatch->ImageBase );
    const tryblock_info *in_catch;
    int *unwind_help;
    UINT i;

    for (i = descr->tryblock_count; i > 0; i--)
    {
        in_catch = cxx_rva( descr->tryblock, dispatch->ImageBase );
        in_catch = &in_catch[i - 1];
        if (trylevel > in_catch->end_level && trylevel <= in_catch->catch_level) break;
    }
    in_catch = i ? in_catch : NULL;

    unwind_help = (int *)(orig_frame + descr->unwind_help);
    if (trylevel > unwind_help[1]) unwind_help[0] = unwind_help[1] = trylevel;
    else trylevel = unwind_help[1];

    for (i = 0; i < descr->tryblock_count; i++)
    {
        const tryblock_info *tryblock = cxx_rva( descr->tryblock, dispatch->ImageBase );
        EXCEPTION_RECORD catch_record;
        CONTEXT ctx;
        void *handler;

        tryblock = &tryblock[i];
        if (trylevel < tryblock->start_level || trylevel > tryblock->end_level) continue;
        if (in_catch)
        {
            if (tryblock->start_level <= in_catch->end_level) continue;
            if (tryblock->end_level > in_catch->catch_level) continue;
        }

        handler = find_catch_handler( object, orig_frame, exc_base, tryblock, info,
                                      dispatch->ImageBase );
        if (!handler) continue;

        cxx_memzero( &catch_record, sizeof(catch_record) );
        catch_record.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
        catch_record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        catch_record.NumberParameters = 11;
        catch_record.ExceptionInformation[0] = (ULONG_PTR)call_catch_block;
        catch_record.ExceptionInformation[1] = orig_frame;
        catch_record.ExceptionInformation[2] = (ULONG_PTR)descr;
        catch_record.ExceptionInformation[3] = tryblock->start_level;
        catch_record.ExceptionInformation[4] = 0;          /* no SEH-translated record, ever (no se_translator) */
        catch_record.ExceptionInformation[5] = (ULONG_PTR)handler;
        catch_record.ExceptionInformation[6] = (ULONG_PTR)rec;
        catch_record.ExceptionInformation[7] = (ULONG_PTR)context;
        catch_record.ExceptionInformation[10] = (ULONG_PTR)-1;
        RtlUnwindEx( (void *)frame, (void *)dispatch->ControlPc, &catch_record, NULL, &ctx, NULL );
        /* RtlUnwindEx does not return on the taken path; falling through
         * means this candidate frame was refused upstream of us (an
         * inconsistency in our own state, not a case to paper over). */
    }
}

static void check_noexcept( const EXCEPTION_RECORD *rec, const cxx_function_descr *descr,
                            BOOL nested )
{
    if (!nested && rec->ExceptionCode == CXX_EXCEPTION &&
        descr->magic >= CXX_FRAME_MAGIC_VC8 && (descr->flags & FUNC_DESCR_NOEXCEPT))
    {
        /* terminate() proper needs ucrtbase's terminate handler chain, which
         * this file has no reason to import just to reproduce a message.
         * A trap at a named guestcrt address IS the termination -- Windows'
         * own noexcept violation is not continuable either. */
        __builtin_trap();
    }
}

/***********************************************************************
 *		__CxxFrameHandler3
 */
EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler3( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                                  CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    const cxx_function_descr *descr =
        cxx_rva( *(UINT *)dispatch->HandlerData, dispatch->ImageBase );
    int trylevel = ip_to_state( descr, dispatch->ControlPc, dispatch->ImageBase );
    ULONG_PTR orig_frame = frame;
    ULONG_PTR throw_base = dispatch->ImageBase;
    void *throw_func;
    cxx_exception_type *exc_type;
    int unwindlevel = -1;
    UINT i, j;

    if (descr->magic < CXX_FRAME_MAGIC_VC6 || descr->magic > CXX_FRAME_MAGIC_VC8)
        return ExceptionContinueSearch;                    /* not ours to decode -- upstream's own FIXME path */

    if (descr->magic >= CXX_FRAME_MAGIC_VC8 && (descr->flags & FUNC_DESCR_SYNCHRONOUS) &&
        rec->ExceptionCode != CXX_EXCEPTION && rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE &&
        rec->ExceptionCode != STATUS_LONGJUMP)
        return ExceptionContinueSearch;

    /* Nested-exception detection: see this file's banner for why
     * dispatch->FunctionEntry replaces a guest RtlLookupFunctionEntry call. */
    throw_func = cxx_rva( dispatch->FunctionEntry->BeginAddress, throw_base );
    for (i = descr->tryblock_count; i > 0; i--)
    {
        const tryblock_info *tryblock = cxx_rva( descr->tryblock, dispatch->ImageBase );
        tryblock = &tryblock[i - 1];
        if (trylevel > tryblock->end_level && trylevel <= tryblock->catch_level)
        {
            for (j = 0; j < tryblock->catchblock_count; j++)
            {
                const catchblock_info *catchblock = cxx_rva( tryblock->catchblock, dispatch->ImageBase );
                catchblock = &catchblock[j];
                if (cxx_rva( catchblock->handler, dispatch->ImageBase ) == throw_func)
                {
                    unwindlevel = tryblock->end_level;
                    orig_frame = *(ULONG_PTR *)(frame + catchblock->frame);
                }
            }
        }
    }

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        BOOL is_consolidate = (rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE &&
                               rec->NumberParameters > 10 &&
                               rec->ExceptionInformation[0] == (ULONG_PTR)call_catch_block);

        if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
            cxx_local_unwind( orig_frame, dispatch, descr,
                              is_consolidate ? (int)rec->ExceptionInformation[3] : trylevel );
        else
            cxx_local_unwind( orig_frame, dispatch, descr, unwindlevel );
        return ExceptionContinueSearch;
    }

    if (!descr->tryblock_count)
    {
        check_noexcept( rec, descr, orig_frame != frame );
        return ExceptionContinueSearch;
    }

    if (rec->ExceptionCode == CXX_EXCEPTION && !rec->ExceptionInformation[1] && !rec->ExceptionInformation[2])
    {
        /* `throw;` -- args[1]==args[2]==0 is cxxthrow.c's own rethrow
         * spelling.  __current_exception() is the SAME per-thread slot
         * __CxxRegisterExceptionObject wrote when the catch we are
         * currently inside was entered, so this substitutes the ORIGINAL
         * exception exactly as upstream's thread_data->exc_record does. */
        EXCEPTION_RECORD **slot = (EXCEPTION_RECORD **)__current_exception();
        if (slot && *slot) *rec = **slot;
    }

    exc_type = (rec->ExceptionCode == CXX_EXCEPTION) ?
        (cxx_exception_type *)rec->ExceptionInformation[2] : NULL;

    find_catch_block( rec, context, frame, dispatch, descr, exc_type, orig_frame );
    check_noexcept( rec, descr, orig_frame != frame );
    return ExceptionContinueSearch;
}

/* __CxxFrameHandler / __CxxFrameHandler2 predate FuncInfo's magic
 * versioning but share its exact table shape (dlls/msvcrt/msvcrt.spec
 * forwards both to __CxxFrameHandler3 for every non-i386 target already);
 * this guest module does the same rather than duplicate the entry point. */
EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                                 CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    return __CxxFrameHandler3( rec, frame, context, dispatch );
}

EXCEPTION_DISPOSITION __cdecl __CxxFrameHandler2( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                                  CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    return __CxxFrameHandler3( rec, frame, context, dispatch );
}
