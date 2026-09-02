/*
 * Media Foundation for x86-64 guests on native ppc64le Wine -- the mfplat-side
 * runtime instance and the flat-export wrappers.
 *
 * This is the native half of the Media Foundation boundary, and it is the
 * same shape as dlls/combase/syscom.c for the same reason: nothing here
 * replaces any Wine implementation.  mfplat/mf/mfreadwrite and the
 * winegstreamer pipeline behind them are Wine's own, and the flat FROM-SPEC
 * thunks already reach them correctly.  Only interface POINTERS crossing to
 * the guest are wrong, and this file is the wrapping layer that fixes them:
 *
 *   * it holds THE ONE winecom runtime instance for the whole Media
 *     Foundation surface (libs/winecom's state is per-linkee, and mfplat.dll
 *     is the module mf.dll and mfreadwrite.dll both import, so the instance
 *     lives here and the siblings reach it through the exported __wine_com_*
 *     helpers and a spec forward of __wine_com_dispatch);
 *
 *   * the host invoker is a DIRECT widest-form native vtable call -- the
 *     implementations are ordinary native PE code in the same Win32 world, so
 *     there is no unixlib on this surface.  That one function pointer is the
 *     entire difference from the d3d12 lane's invoker;
 *
 *   * the flat wrappers (__wine_guest_*) call the real native export through
 *     an ordinary internal call and wrap/translate interface pointers at the
 *     classified positions.  spec2thunk's GUEST-IMPL redirect points the guest
 *     export's native resolution at the wrapper, and the build-time
 *     flat-surface audit refuses to generate if any interface-bearing flat
 *     export is left unclassified -- which is why every one of the 67 in
 *     mfplat.spec appears below, including the ones that only refuse.
 *
 * WHAT IS SERVED AND WHAT IS NOT, in one place.
 *
 * SERVED: the SYNCHRONOUS path.  A guest creates a source reader, sets stream
 * selection and media types, and calls ReadSample in a loop; every object it
 * touches on the way -- IMFSourceReader, IMFMediaType, IMFSample,
 * IMFMediaBuffer, IMFAttributes, IMFByteStream, IMFSourceResolver -- is a
 * winecom proxy whose vtable is the guest module's own trap-stub array.  That
 * is how a large share of shipped games decode a cutscene, and it needs no
 * call in the other direction at all.
 *
 * ALSO SERVED, and this is the half that used to be the headline refusal: the
 * ASYNCHRONOUS path.  Media Foundation's async model is "you implement
 * IMFAsyncCallback, we invoke it", from a work-queue thread the application
 * never created, and that is a guest-implemented COM object being CALLED by
 * native code.  libs/winecom/reverse.c builds the mirror of a proxy for it --
 * a NATIVE vtable whose slots marshal ELFv2 arguments into MS-x64 and enter
 * the guest method through the emulator -- and this surface turns it on
 * (WINECOM_SF_REVERSE below).  So MFPutWorkItem and its fifteen relatives are
 * real wrappers now rather than named refusals, IMFMediaEventGenerator::
 * BeginGetEvent goes through the ordinary marshal table, and setting
 * MF_SOURCE_READER_ASYNC_CALLBACK with IMFAttributes::SetUnknown works because
 * that slot's CA_IFACE_IN row now carries the interface TYPE the reverse
 * direction needs.
 *
 * STILL REFUSED, LOUDLY AND BY NAME: MFAddPeriodicCallback, whose argument is
 * a bare guest FUNCTION POINTER and not an interface at all (a reverse proxy
 * has nothing to build a vtable from; that one wants a spec2thunk callback
 * slot); the cross-surface family, where the interface belongs to another
 * winecom instance; and every signature the marshal tables refuse for what it
 * IS rather than for which direction it travels -- a PROPVARIANT, a by-value
 * GUID, a struct that reaches an interface pointer.  ppc64le/mf/README.md
 * carries what is left.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "objbase.h"
#include "ole2.h"

#include "mfapi.h"
#include "mfidl.h"
#include "mfobjects.h"
#include "mfreadwrite.h"
#include "mftransform.h"
#include "mferror.h"
/* WM_MEDIA_TYPE for the IWMMediaProps hand walkers -- the same IDL struct
 * AM_MEDIA_TYPE is, under the WMSDK name, which is why the representation
 * audit below can use it for both. */
#include "wmsdkidl.h"

#include "wine/debug.h"
#include "wine/winecom.h"
#include "wine/winecom_fpcall.h"
#include "wine/winecom_variant.h"
#include "wine/winecom_selftest.h"

#include "mf_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call: host's vtable slot with up to 16
 * ULONG_PTR arguments (args[0] is `this`).  ELFv2 callees ignore the excess,
 * so one shape serves every slot.  No unixlib: these are ordinary native COM
 * objects in the same process.  A slot whose signature needs a floating-point
 * register is not called through here at all -- it goes through mf_invoke_fp
 * below (PPC64EC step C), driven by the row's fpmask/fpwide/fpret. */
static UINT64 mf_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
{
    void **vtbl = *(void ***)host;

    args[0] = (UINT64)(ULONG_PTR)host;
    return ((UINT64 (*)( ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                         ULONG_PTR ))vtbl[slot])
        ( args[0], args[1], args[2],  args[3],  args[4],  args[5],  args[6],
          args[7], args[8], args[9],  args[10], args[11], args[12], args[13],
          args[14], args[15] );
}

/* The FLOATING-POINT invoker (PPC64EC step C): same direct native vtable
 * call, but through the one shared splitter/caller so a by-value float
 * reaches the other register file.  IMFAttributes::SetDouble and the
 * IMFMediaEngine time/rate surface are the rows this un-refuses -- the
 * marshal table's fpmask/fpwide/fpret say per slot what travels where. */
WINECOM_DEFINE_FP_CALLER( mf_fp_caller )

static UINT64 mf_invoke_fp( void *host, UINT slot, UINT argc, UINT64 *args,
                            UINT fpword, UINT64 *fpret_bits )
{
    void **vtbl = *(void ***)host;

    args[0] = (UINT64)(ULONG_PTR)host;
    return winecom_fp_invoke( mf_fp_caller, vtbl[slot], argc, args, fpword,
                              fpret_bits );
}

/* The three guest thunk modules that publish this roster.  Any one of them
 * being loaded is enough to materialise the vtables; every loaded one is
 * cross-checked, so a stale copy is a load failure rather than a
 * mis-dispatched neighbour slot. */
static const WCHAR *const mf_guest_modules[] =
{
    L"mfplat.dll", L"mf.dll", L"mfreadwrite.dll",
};

/* ------------------------------------------------------- hand-written slots */

/* THE PROPVARIANT FAMILY.  Every PROPVARIANT-bearing slot on the surface is
 * hand-served through the two helpers below, which TRANSLATE the tag instead
 * of refusing the slot: the refusal lives per ARM now, at runtime, naming
 * the VT -- which is what complete means for a tagged union.
 *
 *   IN  (guest -> native): the guest owns the PROPVARIANT and everything it
 *       points at, and nothing here frees any of it -- the callee copies
 *       what it keeps, the ordinary COM in-parameter rule.  Plain tags pass
 *       as the guest's own pointer (payload pointers -- VT_LPWSTR's string,
 *       VT_BLOB's bytes -- are guest memory, which IS host memory on this
 *       port, read with the same 2-byte WCHAR both sides).  VT_UNKNOWN gets
 *       a stack-local shallow copy whose punkVal is the HOST object: one of
 *       our proxies unwraps to its host, a guest-implemented object gets a
 *       reverse proxy or refuses, exactly the CA_IFACE_IN rule -- borrowed
 *       for the call and given back after.
 *
 *   OUT (native -> guest): the callee fills the guest's PROPVARIANT in
 *       place; string/blob payloads it CoTaskMemAllocs transfer WHOLE,
 *       because the guest's eventual PropVariantClear is a thunk that
 *       crosses back into the same native combase -- one process, one
 *       allocator, so the pointer the callee wrote is the pointer the free
 *       will see.  Only the interface arm changes hands: punkVal is wrapped
 *       as an IUnknown proxy (QueryInterface from there is what a COM
 *       caller does with an IUnknown anyway), and winecom_wrap CONSUMES the
 *       callee's reference, so the guest's Clear -> Release balances the
 *       books.
 *
 *   REFUSED ARMS, loudly and per tag: VT_DISPATCH (IDispatch is not on this
 *       roster) and any VECTOR/ARRAY/BYREF modifier on an interface base --
 *       an elementwise translation nobody has needed; the FIXME names the
 *       vt so the day someone does need it, the report says which arm.
 *
 * Argument positions are read straight out of the trap CONTEXT, so the
 * layouts are asserted by the roster and named in gen_winecom.py's
 * HAND_SLOTS comment; nothing infers them.  No hand key on this surface
 * carries a sub-32-bit by-value argument, so the raw slots pass unwidened
 * (the auto rows' narrowmask machinery covers the ones that do). */

struct mf_pv_in_state
{
    PROPVARIANT copy;
    void *borrowed;              /* winecom_to_native borrow to give back */
    void **vec;                  /* a cloned object-vector's element array */
    void **vec_borrowed;         /* ...and the borrows to give back */
    ULONG vec_count;
    const PROPVARIANT *use;      /* what the native callee is handed */
};

static BOOL mf_pv_iface_arm_untranslatable( VARTYPE vt )
{
    VARTYPE base = vt & VT_TYPEMASK;

    /* 2026-09-01: VT_DISPATCH is a SERVED tag (IDispatch is on the roster
     * and Invoke is hand-served), and a VECTOR of VT_UNKNOWN/VT_DISPATCH is
     * served elementwise below.  What remains untranslatable: ARRAY/BYREF
     * object arms (SAFEARRAY element lifetime is oleaut's, not a walker's)
     * and vectors of VARIANTs (each element re-dispatches on its own tag,
     * recursively -- add it when something real carries one). */
    if ((vt & (VT_ARRAY | VT_BYREF)) &&
        (base == VT_UNKNOWN || base == VT_DISPATCH || base == VT_VARIANT))
        return TRUE;
    if ((vt & VT_VECTOR) && base == VT_VARIANT) return TRUE;
    return FALSE;
}

/* the vector element view of a PROPVARIANT's CAUNK arm: count + elements */
static ULONG mf_pv_vec_count( const PROPVARIANT *pv ) { return pv->caub.cElems; }
static IUnknown **mf_pv_vec_elems( const PROPVARIANT *pv )
{
    return (IUnknown **)pv->caub.pElems;
}

static BOOL mf_pv_is_obj_vector( VARTYPE vt )
{
    VARTYPE base = vt & VT_TYPEMASK;
    return (vt & VT_VECTOR) && !(vt & (VT_ARRAY | VT_BYREF)) &&
           (base == VT_UNKNOWN || base == VT_DISPATCH);
}

static void mf_pv_in_end( struct mf_pv_in_state *st );

static BOOL mf_pv_in( const PROPVARIANT *pv, struct mf_pv_in_state *st, UINT slot )
{
    static LONG logged;

    st->borrowed = NULL;
    st->vec = st->vec_borrowed = NULL;
    st->vec_count = 0;
    st->use = pv;
    if (!pv) return TRUE;
    if (mf_pv_iface_arm_untranslatable( pv->vt ))
    {
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "mf: slot %u was given a PROPVARIANT of type 0x%04x -- an "
                   "interface arm nothing can translate (an ARRAY/BYREF of "
                   "objects, or a vector of VARIANTs).  Refusing this call; "
                   "every other tag is served.\n", slot, pv->vt );
        return FALSE;
    }
    if (pv->vt == VT_UNKNOWN || pv->vt == VT_DISPATCH)
    {
        st->copy = *pv;
        if (pv->punkVal)
        {
            void *native;
            if (!winecom_to_native( pv->punkVal, MF_IFACE_IUnknown, &native ))
            {
                WARN( "mf: slot %u: object payload %p (vt 0x%04x) has no "
                      "native identity and no reverse proxy; refusing\n",
                      slot, pv->punkVal, pv->vt );
                return FALSE;
            }
            st->copy.punkVal = native;
            st->borrowed = native;
        }
        st->use = &st->copy;
    }
    else if (mf_pv_is_obj_vector( pv->vt ))
    {
        /* clone the element array; each guest element unwraps to its host
         * object, borrowed for the call's duration.  The guest's own array
         * is never mutated. */
        ULONG n = mf_pv_vec_count( pv ), i;
        IUnknown **src = mf_pv_vec_elems( pv );

        st->copy = *pv;
        if (n && src)
        {
            st->vec = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       2 * n * sizeof(void *) );
            if (!st->vec) return FALSE;
            st->vec_borrowed = st->vec + n;
            st->vec_count = n;
            for (i = 0; i < n; i++)
            {
                void *native = NULL;

                if (src[i] &&
                    !winecom_to_native( src[i], MF_IFACE_IUnknown, &native ))
                {
                    WARN( "mf: slot %u: object vector element %u has no "
                          "native identity; refusing\n", slot, i );
                    mf_pv_in_end( st );
                    return FALSE;
                }
                st->vec[i] = native;
                st->vec_borrowed[i] = native;
            }
            st->copy.caub.pElems = (UCHAR *)st->vec;
        }
        st->use = &st->copy;
    }
    return TRUE;
}

static void mf_pv_in_end( struct mf_pv_in_state *st )
{
    ULONG i;

    if (st->borrowed) winecom_to_native_end( st->borrowed );
    for (i = 0; i < st->vec_count; i++)
        if (st->vec_borrowed[i]) winecom_to_native_end( st->vec_borrowed[i] );
    if (st->vec) RtlFreeHeap( GetProcessHeap(), 0, st->vec );
    st->vec = st->vec_borrowed = NULL;
    st->vec_count = 0;
}

/* Only meaningful after a SUCCEEDED call wrote *pv. */
static HRESULT mf_pv_out_fixup( PROPVARIANT *pv, UINT slot )
{
    static LONG logged;

    if (!pv) return S_OK;
    if (mf_pv_iface_arm_untranslatable( pv->vt ))
    {
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "mf: slot %u returned a PROPVARIANT of type 0x%04x -- an "
                   "interface arm nothing can translate; clearing it rather "
                   "than handing the guest a native vtable.\n", slot, pv->vt );
        PropVariantClear( pv );
        return E_NOTIMPL;
    }
    if ((pv->vt == VT_UNKNOWN || pv->vt == VT_DISPATCH) && pv->punkVal)
    {
        void *guest = winecom_wrap( pv->punkVal,
                                    pv->vt == VT_DISPATCH ? MF_IFACE_IDispatch
                                                          : MF_IFACE_IUnknown );
        if (!guest)
        {
            /* wrap released the object rather than hand out a raw vtable;
             * scrub the arm so the guest never sees the host pointer. */
            pv->punkVal = NULL;
            pv->vt = VT_EMPTY;
            return E_NOTIMPL;
        }
        pv->punkVal = guest;
    }
    else if (mf_pv_is_obj_vector( pv->vt ))
    {
        /* wrap each element in place: the vector's storage is the callee's
         * CoTaskMemAlloc'd answer and now belongs to the guest, whose
         * PropVariantClear releases each non-NULL element -- a wrapped
         * proxy releases correctly, a scrubbed NULL is skipped. */
        ULONG n = mf_pv_vec_count( pv ), i;
        IUnknown **el = mf_pv_vec_elems( pv );
        UINT idx = (pv->vt & VT_TYPEMASK) == VT_DISPATCH ? MF_IFACE_IDispatch
                                                         : MF_IFACE_IUnknown;

        for (i = 0; el && i < n; i++)
            if (el[i]) el[i] = winecom_wrap( el[i], idx );
    }
    return S_OK;
}

/* (this, <plain>, <PROPVARIANT in> @2) -- IMFSourceReader::SetCurrentPosition,
 * IMFMediaSession::Start, IMFAttributes::SetItem, IMFMetadata::SetProperty. */
static UINT64 hand_propvariant_in( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st;
    UINT64 ret;

    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
                   &st, slot ))
        return (UINT64)(UINT)E_NOTIMPL;
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)st.use;
    ret = mf_invoke( host, slot, 3, args );
    mf_pv_in_end( &st );
    return ret;
}

/* (this, <plain>, <plain>, <PROPVARIANT out> @3) --
 * IMFSourceReader::GetPresentationAttribute, IMFAttributes::GetItemByIndex,
 * IMFMediaEngineEx::GetStreamAttribute. */
static UINT64 hand_propvariant_out( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    PROPVARIANT *pv;
    HRESULT hr;

    pv = (PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = winecom_read_arg( ctx, 2 );
    args[3] = (UINT64)(ULONG_PTR)pv;
    hr = (HRESULT)mf_invoke( host, slot, 4, args );
    if (SUCCEEDED(hr)) hr = mf_pv_out_fixup( pv, slot );
    return (UINT64)(UINT)hr;
}

/* (this, <PROPVARIANT out> @1) -- IMFMediaEvent::GetValue,
 * IMFMetadata::GetAllLanguages / GetAllPropertyNames.  GetValue is the one
 * that made translation (rather than the old audit) necessary: a session's
 * MESessionTopologySet event carries its IMFTopology as VT_UNKNOWN, so
 * "refuse the interface arm" refused the event loop itself. */
static UINT64 hand_pv_out_1( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    PROPVARIANT *pv;
    HRESULT hr;

    pv = (PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    args[1] = (UINT64)(ULONG_PTR)pv;
    hr = (HRESULT)mf_invoke( host, slot, 2, args );
    if (SUCCEEDED(hr)) hr = mf_pv_out_fixup( pv, slot );
    return (UINT64)(UINT)hr;
}

/* (this, <plain>, <PROPVARIANT out> @2) -- IMFAttributes::GetItem,
 * IMFMediaEngineEx::GetPresentationAttribute / GetStatistics,
 * IMFMetadata::GetProperty. */
static UINT64 hand_pv_out_2( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    PROPVARIANT *pv;
    HRESULT hr;

    pv = (PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)pv;
    hr = (HRESULT)mf_invoke( host, slot, 3, args );
    if (SUCCEEDED(hr)) hr = mf_pv_out_fixup( pv, slot );
    return (UINT64)(UINT)hr;
}

/* (this, <plain>, <PROPVARIANT in> @2, <plain out> @3) --
 * IMFAttributes::CompareItem; the BOOL* is plain guest memory. */
static UINT64 hand_pv_in_mid( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st;
    UINT64 ret;

    /* REFUSAL HYGIENE, BY HAND, because no generated scrub mask reaches a
     * WINECOM_F_HAND row or a flat GUEST-IMPL wrapper: both own their
     * out-params the way scrub_refused_outs() owns a table refusal's.  Every
     * refusal below scrubs what it can prove -- interface/pointer cells to
     * NULL, PROPVARIANT/VARIANT to zeroed (VT_EMPTY is 0), counts and keys to
     * 0 -- through winecom_refused_scrub_*, which honours WINEEMUNOREFUSESCRUB
     * so the hygiene gate's sabotage arm can prove the write load-bearing.
     * [MEASURED] dlls/combase/syscom.c's IMMDevice::Activate is the site that
     * cost days: the Witcher 3 read a never-written *ppv off its own stack and
     * the emulator decoded a host module's ppc64le bytes as x86.  Native
     * failures stay untouched -- real MF leaves *out alone on failure, and
     * matching Windows means scrubbing only the refusals this side invented.
     * A buffer whose length the caller already knows is left alone too; the
     * one out this file deliberately does not scrub is named where it is. */
    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
                   &st, slot ))
    {
        winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 3 ) );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)st.use;
    args[3] = winecom_read_arg( ctx, 3 );
    ret = mf_invoke( host, slot, 4, args );
    mf_pv_in_end( &st );
    return ret;
}

/* (this, <plain> x3, <PROPVARIANT in> @4) -- IMFMediaEventGenerator::
 * QueueEvent and IMFMediaEventQueue::QueueEventParamVar. */
static UINT64 hand_pv_in_4( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st;
    UINT64 ret;

    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 4 ),
                   &st, slot ))
        return (UINT64)(UINT)E_NOTIMPL;
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = winecom_read_arg( ctx, 2 );
    args[3] = winecom_read_arg( ctx, 3 );
    args[4] = (UINT64)(ULONG_PTR)st.use;
    ret = mf_invoke( host, slot, 5, args );
    mf_pv_in_end( &st );
    return ret;
}

/* (this, <plain>, <PROPVARIANT in> @2, <PROPVARIANT in> @3) --
 * IMFStreamSink::PlaceMarker (marker value + context value). */
static UINT64 hand_pv_in_2_3( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st2, st3;
    UINT64 ret;

    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
                   &st2, slot ))
        return (UINT64)(UINT)E_NOTIMPL;
    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 3 ),
                   &st3, slot ))
    {
        mf_pv_in_end( &st2 );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)st2.use;
    args[3] = (UINT64)(ULONG_PTR)st3.use;
    ret = mf_invoke( host, slot, 4, args );
    mf_pv_in_end( &st3 );
    mf_pv_in_end( &st2 );
    return ret;
}

/* (this, IMFPresentationDescriptor* @1, const GUID* @2, PROPVARIANT in @3)
 * -- IMFMediaSource::Start: the one PROPVARIANT slot that also carries an
 * interface IN-parameter, unwrapped by the same CA_IFACE_IN rule the auto
 * rows use (NULL stays NULL). */
static UINT64 hand_source_start( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st;
    void *desc_native;
    UINT64 ret;

    if (!winecom_to_native( (void *)(ULONG_PTR)winecom_read_arg( ctx, 1 ),
                            MF_IFACE_IMFPresentationDescriptor, &desc_native ))
    {
        WARN( "mf: Start's descriptor has no native identity; refusing\n" );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 3 ),
                   &st, slot ))
    {
        winecom_to_native_end( desc_native );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    args[1] = (UINT64)(ULONG_PTR)desc_native;
    args[2] = winecom_read_arg( ctx, 2 );
    args[3] = (UINT64)(ULONG_PTR)st.use;
    ret = mf_invoke( host, slot, 4, args );
    mf_pv_in_end( &st );
    winecom_to_native_end( desc_native );
    return ret;
}

/* (this, const GUID* @1, PROPVARIANT in @2, PROPVARIANT out @3,
 * PROPVARIANT out @4) -- IMFSeekInfo::GetNearestKeyFrames. */
static UINT64 hand_pv_keyframes( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    struct mf_pv_in_state st;
    PROPVARIANT *prev, *next;
    HRESULT hr;

    prev = (PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    next = (PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 4 );
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!mf_pv_in( (const PROPVARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
                   &st, slot ))
    {
        winecom_refused_scrub_mem( prev, sizeof(*prev) );
        winecom_refused_scrub_mem( next, sizeof(*next) );
        return (UINT64)(UINT)E_NOTIMPL;
    }
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)st.use;
    args[3] = (UINT64)(ULONG_PTR)prev;
    args[4] = (UINT64)(ULONG_PTR)next;
    hr = (HRESULT)mf_invoke( host, slot, 5, args );
    mf_pv_in_end( &st );
    if (SUCCEEDED(hr)) hr = mf_pv_out_fixup( prev, slot );
    if (SUCCEEDED(hr)) hr = mf_pv_out_fixup( next, slot );
    return (UINT64)(UINT)hr;
}

/* THE WM_MEDIA_TYPE FAMILY.  The struct's pUnk member is AUDITED rather
 * than translated, and the audit is a provable no-op today: the only native
 * producer zeroes it (dlls/mfplat/mediatype.c:4351, the memset in
 * MFInitAMMediaTypeFromMFMediaType) and no assignment site exists anywhere
 * in mfplat, wmvcore or winegstreamer outside tests.  A non-NULL pUnk in
 * either direction is therefore the impossible-today case, refused loudly
 * so the day an implementation changes, the report names it here instead of
 * a guest calling through a native vtable.  pbFormat is a plain data
 * pointer into memory both sides read. */

static BOOL mf_wmt_in_ok( const WM_MEDIA_TYPE *mt, UINT slot )
{
    static LONG logged;

    if (!mt || !mt->pUnk) return TRUE;
    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "mf: slot %u was given a WM_MEDIA_TYPE whose pUnk is %p; no "
               "native consumer reads it and nothing translates it -- "
               "refusing\n", slot, mt->pUnk );
    return FALSE;
}

static HRESULT mf_wmt_out_fixup( WM_MEDIA_TYPE *mt, UINT slot )
{
    static LONG logged;

    if (!mt || !mt->pUnk) return S_OK;
    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "mf: slot %u produced a WM_MEDIA_TYPE with a live pUnk %p, "
               "which dlls/mfplat/mediatype.c never does; releasing it and "
               "refusing rather than handing the guest a native vtable\n",
               slot, mt->pUnk );
    IUnknown_Release( mt->pUnk );
    mt->pUnk = NULL;
    return E_NOTIMPL;
}

/* (this, WM_MEDIA_TYPE *out @1, DWORD *inout @2) -- IWMMediaProps::
 * GetMediaType; pType may be NULL for the size query. */
static UINT64 hand_wmt_out( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    WM_MEDIA_TYPE *mt;
    HRESULT hr;

    mt = (WM_MEDIA_TYPE *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    args[1] = (UINT64)(ULONG_PTR)mt;
    args[2] = winecom_read_arg( ctx, 2 );
    hr = (HRESULT)mf_invoke( host, slot, 3, args );
    if (SUCCEEDED(hr)) hr = mf_wmt_out_fixup( mt, slot );
    return (UINT64)(UINT)hr;
}

/* (this, WM_MEDIA_TYPE *in @1) -- IWMMediaProps::SetMediaType. */
static UINT64 hand_wmt_in_1( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    const WM_MEDIA_TYPE *mt;

    mt = (const WM_MEDIA_TYPE *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    if (!mf_wmt_in_ok( mt, slot )) return (UINT64)(UINT)E_NOTIMPL;
    args[1] = (UINT64)(ULONG_PTR)mt;
    return mf_invoke( host, slot, 2, args );
}

/* (this, <plain> @1, WM_MEDIA_TYPE *in @2) -- IWMReaderAccelerator::Notify. */
static UINT64 hand_wmt_in_2( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    const WM_MEDIA_TYPE *mt;

    mt = (const WM_MEDIA_TYPE *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    if (!mf_wmt_in_ok( mt, slot )) return (UINT64)(UINT)E_NOTIMPL;
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)mt;
    return mf_invoke( host, slot, 3, args );
}

/* (this, <plain> @1, WM_MEDIA_TYPE *in @2, void* @3) --
 * IWMReaderCallbackAdvanced::OnOutputPropsChanged. */
static UINT64 hand_wmt_in_2_ctx( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    const WM_MEDIA_TYPE *mt;

    mt = (const WM_MEDIA_TYPE *)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    if (!mf_wmt_in_ok( mt, slot )) return (UINT64)(UINT)E_NOTIMPL;
    args[1] = winecom_read_arg( ctx, 1 );
    args[2] = (UINT64)(ULONG_PTR)mt;
    args[3] = winecom_read_arg( ctx, 3 );
    return mf_invoke( host, slot, 4, args );
}

/* THE REPRESENTATION TRIO.  A by-value GUID crosses MS-x64 as a HIDDEN
 * POINTER to a caller temporary (any aggregate that is not 1/2/4/8 bytes),
 * so the guest's argument slot holds an address; ELFv2 wants the sixteen
 * bytes in registers, which a real by-value prototype provides once the
 * pointer is dereferenced.  That is the whole trick: read the slot, deref,
 * call with the right C type.
 *
 * The blob GetRepresentation yields is always an AM_MEDIA_TYPE --
 * dlls/mfplat/mediatype.c routes all five supported representation GUIDs
 * through MFCreateAMMediaTypeFromMFMediaType and fails every other GUID
 * with MF_E_UNSUPPORTED_REPRESENTATION -- so the pUnk audit above applies
 * (WM_MEDIA_TYPE and AM_MEDIA_TYPE are the same IDL struct).  On the
 * impossible-today live pUnk the blob is freed the way FreeRepresentation
 * frees it (CoTaskMemFree of pbFormat then of the blob -- mediatype.c),
 * because the guest never saw the pointer and could not free it itself.
 * FreeRepresentation itself never reads pUnk, so it is a plain
 * dereference-and-call. */

static UINT64 hand_get_representation( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void **vtbl = *(void ***)host;
    const GUID *rep = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void **out = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    HRESULT hr;

    hr = ((HRESULT (*)( void *, GUID, void ** ))vtbl[slot])( host, *rep, out );
    if (SUCCEEDED(hr) && out && *out)
    {
        WM_MEDIA_TYPE *mt = *out;
        if (FAILED(mf_wmt_out_fixup( mt, slot )))
        {
            CoTaskMemFree( mt->pbFormat );
            CoTaskMemFree( mt );
            *out = NULL;
            return (UINT64)(UINT)E_NOTIMPL;
        }
    }
    return (UINT64)(UINT)hr;
}

static UINT64 hand_free_representation( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void **vtbl = *(void ***)host;
    const GUID *rep = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void *blob = (void *)(ULONG_PTR)winecom_read_arg( ctx, 2 );

    return (UINT64)(UINT)((HRESULT (*)( void *, GUID, void * ))vtbl[slot])
        ( host, *rep, blob );
}

/* (this, GUID byval @1, LPVOID* @2, LONG @3) -- IMFVideoMediaType::
 * GetVideoRepresentation, the evr spelling of the same shape with a stride
 * argument; same audit for the same reason. */
static UINT64 hand_get_video_representation( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void **vtbl = *(void ***)host;
    const GUID *rep = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );
    void **out = (void **)(ULONG_PTR)winecom_read_arg( ctx, 2 );
    LONG stride = (LONG)winecom_read_arg( ctx, 3 );
    HRESULT hr;

    hr = ((HRESULT (*)( void *, GUID, void **, LONG ))vtbl[slot])
        ( host, *rep, out, stride );
    if (SUCCEEDED(hr) && out && *out)
    {
        WM_MEDIA_TYPE *mt = *out;
        if (FAILED(mf_wmt_out_fixup( mt, slot )))
        {
            CoTaskMemFree( mt->pbFormat );
            CoTaskMemFree( mt );
            *out = NULL;
            return (UINT64)(UINT)E_NOTIMPL;
        }
    }
    return (UINT64)(UINT)hr;
}

/* (this, DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER *samples,
 * DWORD *status) -- IMFTransform::ProcessOutput.  Every element carries an
 * IN/OUT IMFSample* (the caller's own buffer for most MFTs; NULL going in and
 * callee-allocated coming out for the ones that manage their own, e.g.
 * D3D-aware decoders) and an OUT-only IMFCollection* of stream events, and
 * neither is visible in the vtable signature -- ppc64le/mf/gen_winecom.py's
 * struct-bearing scan is what catches it and routes it here instead of
 * refusing the whole method.  Same shallow-copy-the-array shape as
 * dlls/d3d12/main.c's hand_resource_barrier; the translation is per FIELD
 * rather than per union arm because MFT_OUTPUT_DATA_BUFFER has no
 * discriminant to switch on. */
static UINT64 hand_process_output( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    DWORD count = (DWORD)winecom_read_arg( ctx, 2 );
    MFT_OUTPUT_DATA_BUFFER *src = (MFT_OUTPUT_DATA_BUFFER *)(ULONG_PTR)winecom_read_arg( ctx, 3 );
    UINT64 args[16] = { 0 };
    MFT_OUTPUT_DATA_BUFFER *copy = NULL;
    void **borrowed = NULL;
    HRESULT hr;
    DWORD i;

    if (count && src)
    {
        /* refusal hygiene by hand -- see hand_pv_in_mid.  Only pdwStatus: the
         * MFT_OUTPUT_DATA_BUFFER array is IN-OUT and already holds the guest's
         * own values, so scrubbing it would destroy the caller's input. */
        if (!(copy = calloc( count, sizeof(*copy) )) ||
            !(borrowed = calloc( count, sizeof(*borrowed) )))
        {
            free( copy );
            winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 4 ) );
            return (UINT64)(UINT)E_OUTOFMEMORY;
        }
        memcpy( copy, src, count * sizeof(*copy) );
        for (i = 0; i < count; i++)
        {
            /* Same classifier an ordinary CA_IFACE_IN slot would use: one of
             * our proxies unwraps to its host, NULL stays NULL, and a
             * guest-implemented object gets a reverse proxy or -- since
             * MFCreateSample is the only way a guest ever gets an IMFSample
             * in the first place -- refuses.  Cannot actually happen on this
             * surface, but it is the fail-closed answer if it ever did. */
            if (!winecom_to_native( copy[i].pSample, MF_IFACE_IMFSample,
                                    (void **)&copy[i].pSample ))
            {
                WARN( "ProcessOutput: sample %lu (%p) has no reverse proxy; "
                      "refusing the call\n", i, src[i].pSample );
                while (i--) winecom_to_native_end( borrowed[i] );
                free( borrowed );
                free( copy );
                winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 4 ) );
                return (UINT64)(UINT)E_NOTIMPL;
            }
            borrowed[i] = copy[i].pSample;
            copy[i].pEvents = NULL;      /* OUT only; the callee fills it in */
        }
    }

    args[1] = winecom_read_arg( ctx, 1 );           /* flags */
    args[2] = count;
    args[3] = (UINT64)(ULONG_PTR)(copy ? copy : src);
    args[4] = winecom_read_arg( ctx, 4 );            /* status */
    hr = (HRESULT)mf_invoke( host, slot, 5, args );

    /* COPIED BACK WHATEVER hr SAYS, and that is not laxness.  ProcessOutput's
     * most important non-S_OK answer is MF_E_TRANSFORM_STREAM_CHANGE, which
     * is a FAILED() hresult and an entirely routine one: an H.264 decoder
     * returns it the moment the stream's format changes, and the caller is
     * required to read dwStatus to find WHICH stream carries the
     * MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE bit before it can renegotiate the
     * type.  Gating the write-back on SUCCEEDED() left every one of those
     * bits behind, so a guest saw the failure code and nothing to act on --
     * and any sample the transform had already allocated into the array went
     * unwrapped and unreferenced.  On a hard failure the callee leaves the
     * entries as it found them, so copying them back writes the same
     * proxies the guest passed in: winecom_wrap is keyed on the host pointer
     * and hands back the object already interned for it. */
    if (copy)
    {
        for (i = 0; i < count; i++)
        {
            winecom_to_native_end( borrowed[i] );
            src[i].dwStatus = copy[i].dwStatus;
            src[i].pSample = copy[i].pSample
                ? winecom_wrap( copy[i].pSample, MF_IFACE_IMFSample ) : NULL;
            src[i].pEvents = copy[i].pEvents
                ? winecom_wrap( copy[i].pEvents, MF_IFACE_IMFCollection ) : NULL;
        }
        free( borrowed );
        free( copy );
    }
    return (UINT64)(UINT)hr;
}

/* (this, GUID byval @1, void* @2, DWORD* @3) -- INSSBuffer3::GetProperty:
 * the property GUID by hidden pointer, a plain payload buffer and its size
 * cell.  And the Set twin, whose @3 is the size BY VALUE. */
static UINT64 hand_nss_get_property( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void **vtbl = *(void ***)host;
    const GUID *prop = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    return (UINT64)(UINT)((HRESULT (*)( void *, GUID, void *, DWORD * ))vtbl[slot])
        ( host, *prop, (void *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
          (DWORD *)(ULONG_PTR)winecom_read_arg( ctx, 3 ) );
}

static UINT64 hand_nss_set_property( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    void **vtbl = *(void ***)host;
    const GUID *prop = (const GUID *)(ULONG_PTR)winecom_read_arg( ctx, 1 );

    return (UINT64)(UINT)((HRESULT (*)( void *, GUID, void *, DWORD ))vtbl[slot])
        ( host, *prop, (void *)(ULONG_PTR)winecom_read_arg( ctx, 2 ),
          (DWORD)winecom_read_arg( ctx, 3 ) );
}

/* The order here IS gen_winecom.py's HAND_SLOTS order (first appearance). */
/* IDispatch::Invoke (2026-09-01): the shared VARIANT/DISPPARAMS discipline,
 * include/wine/winecom_variant.h -- the guest's rgvarg is cloned and each
 * element's object arm unwrapped; the result VARIANT is wrapped (or
 * scrubbed, loudly) under this surface's own IDispatch row. */
/* refusal hygiene by hand -- see hand_pv_in_mid.  Invoke's three outs, the
 * syscom twin's treatment verbatim: a zeroed VARIANT/EXCEPINFO is the valid
 * empty value, so an unchecked caller VariantClear()s nothing. */
static void mf_invoke_scrub_outs( AMD64_CONTEXT *ctx )
{
    winecom_refused_scrub_mem( (void *)(ULONG_PTR)winecom_read_arg( ctx, 6 ),
                               sizeof(VARIANT) );
    winecom_refused_scrub_mem( (void *)(ULONG_PTR)winecom_read_arg( ctx, 7 ),
                               sizeof(EXCEPINFO) );
    winecom_refused_scrub_dw( (void *)(ULONG_PTR)winecom_read_arg( ctx, 8 ) );
}

static UINT64 hand_dispatch_invoke( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    UINT64 args[16] = { 0 };
    DISPPARAMS *dp = (DISPPARAMS *)(ULONG_PTR)winecom_read_arg( ctx, 5 );
    VARIANT *result = (VARIANT *)(ULONG_PTR)winecom_read_arg( ctx, 6 );
    VARIANTARG local[16], *cloned = local;
    DISPPARAMS use;
    static LONG logged;
    UINT64 ret;
    UINT i;

    for (i = 1; i < 9; i++) args[i] = winecom_read_arg( ctx, i );
    if (dp && dp->cArgs)
    {
        use = *dp;
        if (dp->cArgs > ARRAYSIZE(local))
        {
            cloned = RtlAllocateHeap( GetProcessHeap(), 0,
                                      dp->cArgs * sizeof(*cloned) );
            if (!cloned)
            {
                mf_invoke_scrub_outs( ctx );
                return (UINT64)(UINT)E_OUTOFMEMORY;
            }
        }
        memcpy( cloned, dp->rgvarg, dp->cArgs * sizeof(*cloned) );
        for (i = 0; i < dp->cArgs; i++)
            if (!winecom_variant_in( &cloned[i] ))
            {
                if (!InterlockedExchange( &logged, 1 ))
                    FIXME( "mf: Invoke was given a VARIANT (vt 0x%04x) "
                           "carrying a guest-authored object or an "
                           "untranslatable arm; refusing\n",
                           V_VT(&dp->rgvarg[i]) );
                if (cloned != local)
                    RtlFreeHeap( GetProcessHeap(), 0, cloned );
                mf_invoke_scrub_outs( ctx );
                return (UINT64)(UINT)E_NOTIMPL;
            }
        use.rgvarg = cloned;
        args[5] = (UINT64)(ULONG_PTR)&use;
    }
    ret = mf_invoke( host, slot, 9, args );
    if (dp && dp->cArgs && cloned != local)
        RtlFreeHeap( GetProcessHeap(), 0, cloned );
    if (SUCCEEDED((HRESULT)(UINT)ret) &&
        winecom_variant_out_fixup( result, MF_IFACE_IDispatch ) &&
        !InterlockedExchange( &logged, 1 ))
        FIXME( "mf: Invoke returned a non-IDispatch object in a VARIANT; "
               "released and scrubbed to EMPTY\n" );
    return ret;
}

static const winecom_hand_fn mf_hand_funcs[] =
{
    hand_propvariant_in,
    hand_propvariant_out,
    hand_process_output,
    hand_pv_out_1,
    hand_pv_out_2,
    hand_pv_in_mid,
    hand_pv_in_4,
    hand_pv_in_2_3,
    hand_source_start,
    hand_pv_keyframes,
    hand_wmt_out,
    hand_wmt_in_1,
    hand_wmt_in_2,
    hand_wmt_in_2_ctx,
    hand_get_representation,
    hand_free_representation,
    hand_get_video_representation,
    hand_nss_get_property,
    hand_nss_set_property,
    hand_dispatch_invoke,
};

static const struct winecom_surface mf_surface =
{
    .name = "mf",
    .guest_modules = mf_guest_modules,
    .module_count = ARRAYSIZE(mf_guest_modules),
    .ifaces = mf_com_ifaces,
    .iface_count = MF_IFACE_COUNT,
    .invoke = mf_invoke,
    .hand_funcs = mf_hand_funcs,
    .hand_count = ARRAYSIZE(mf_hand_funcs),
    /* THE REVERSE DIRECTION IS ON for this surface.  mf_invoke calls a native
     * PE vtable directly -- there is no unixlib on this lane -- so a REVERSE
     * PROXY, which is a PE-side object whose slots enter guest code through
     * the emulator, is a thing native Media Foundation can be handed and call.
     * (The d3d12 lane's invoker crosses a unixlib and must never get one,
     * which is why this is a per-surface opt-in and not a global.) */
    .flags = WINECOM_SF_REVERSE,
    .invoke_fp = mf_invoke_fp,
};

C_ASSERT( MF_HAND_COUNT == ARRAYSIZE(mf_hand_funcs) );

static BOOL mf_ready( void )
{
    return winecom_attach( &mf_surface );
}

/* ---------------------------------------------------- exported dispatch */

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!mf_ready()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never on
 * a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* The sibling-module helper API: mf.dll's and mfreadwrite.dll's wrappers
 * reach the single runtime instance through these forwards, never by
 * re-linking libwinecom (which would give them their own tables and their own
 * intern space, so a proxy minted by one would be a stranger to the other). */
void *WINAPI __wine_com_wrap( void *host, UINT iface )
{
    if (!mf_ready()) return NULL;
    return winecom_wrap( host, iface );
}

void *WINAPI __wine_com_unwrap( void *proxy )
{
    return winecom_unwrap( proxy );
}

BOOL WINAPI __wine_com_translate_in( void *guest_seen, void **host_out )
{
    if (!mf_ready())
    {
        *host_out = NULL;
        return FALSE;
    }
    return winecom_translate_in( guest_seen, host_out );
}

HRESULT WINAPI __wine_com_wrap_out_iface( HRESULT hr, const GUID *riid, void **ppv )
{
    if (!mf_ready()) return hr;
    return winecom_wrap_out_iface( hr, riid, ppv );
}

void WINAPI __wine_com_wrap_static( void **p, UINT iface )
{
    if (!mf_ready()) return;
    winecom_wrap_static( p, iface );
}

UINT WINAPI __wine_com_iface_from_iid( const GUID *riid )
{
    if (!mf_ready()) return ~0u;
    return winecom_iface_from_iid( riid );
}

/* ------------------------------------------------- the two refusal shapes */

/* THE UNTYPED TRANSLATE-IN, kept exactly as it was.  An interface-typed IN
 * parameter is never blindly unwrapped: one of our proxies unwraps to its host
 * pointer, NULL stays NULL, and anything else is an object the GUEST
 * implemented.  This spelling carries no interface TYPE, so a guest-
 * implemented object still has nothing to build a reverse proxy's slot table
 * out of and is still refused, by name.
 *
 * dlls/mf and dlls/mfreadwrite reach this through a spec forward and every one
 * of their wrappers passes an object the guest got FROM Media Foundation --
 * an IMFMediaType, an IMFByteStream, an IMFAttributes -- so the untyped form
 * is the right one there and the refusal is the honest answer for the rest.
 * The typed sibling below is what the async family uses. */
BOOL WINAPI __wine_mf_translate_in( LONG *logged, const char *export_name,
                                    const char *iface_name, void *obj,
                                    void **host_out )
{
    if (__wine_com_translate_in( obj, host_out )) return TRUE;
    if (!InterlockedExchange( logged, 1 ))
        FIXME( "mf: %s was handed a GUEST-IMPLEMENTED %s (%p), and this export "
               "has no interface TYPE recorded for that position -- so there "
               "is no roster slot table to give a reverse proxy.  Refusing "
               "rather than handing native MF an x86-64 vtable; if this export "
               "turns out to matter, give it a __wine_mf_translate_in_iface "
               "call with the roster index.\n", export_name, iface_name, obj );
    return FALSE;
}

/* THE TYPED TRANSLATE-IN: the same classifier with the interface named, which
 * is what lets a guest-implemented object become a REVERSE PROXY -- a native
 * vtable whose slots enter guest code through the emulator.  This is the whole
 * of Media Foundation's async model working:
 *
 *   guest writes an IMFAsyncCallback  ->  MFPutWorkItem  ->  this  ->
 *   a native object rtworkq queues and later Invoke()s on ITS OWN worker
 *   thread  ->  libs/winecom/reverse.c marshals and enters the guest method,
 *   with the native IMFAsyncResult it was given arriving as a forward proxy.
 *
 * The native pointer is BORROWED for the duration of the call: every caller
 * pairs this with __wine_mf_translate_in_end, so an export that took its own
 * reference (MFPutWorkItem does) keeps the object alive and one that did not
 * lets it go. */
BOOL WINAPI __wine_mf_translate_in_iface( LONG *logged, const char *export_name,
                                          const char *iface_name, UINT iface,
                                          void *obj, void **host_out )
{
    if (!mf_ready())
    {
        *host_out = NULL;
        return FALSE;
    }
    if (winecom_to_native( obj, iface, host_out )) return TRUE;
    if (!InterlockedExchange( logged, 1 ))
        FIXME( "mf: %s could not give the %s at %p a reverse proxy; refusing "
               "rather than handing native MF an x86-64 vtable\n",
               export_name, iface_name, obj );
    return FALSE;
}

void WINAPI __wine_mf_translate_in_end( void *host )
{
    winecom_to_native_end( host );
}

/* THE CROSS-SURFACE GAP.  libs/winecom's state is per-linkee by design, so a
 * proxy minted by combase's system-COM instance (an IStream) or by the d3d11
 * lane's (an ID3D11Texture2D) is not one of THIS runtime's proxies and would
 * be refused one frame later, in the middle of a wrap.  Refused here, where
 * the reason is legible. */
HRESULT WINAPI __wine_mf_refuse_cross_surface( LONG *logged, const char *export_name,
                                               const char *iface_name,
                                               const char *owner )
{
    if (!InterlockedExchange( logged, 1 ))
        FIXME( "mf: %s takes a %s, which belongs to the %s winecom surface. "
               "libs/winecom state is per-linkee: a proxy from that surface "
               "is not one of this runtime's proxies, and wrapping it here "
               "would intern the same host object twice with two different "
               "vtables.  Refusing until the surfaces share an intern "
               "space.\n", export_name, iface_name, owner );
    return E_NOTIMPL;
}

/* A PROPVARIANT the callee FILLED IN.  The struct itself crosses fine -- both
 * sides compile it from the same Wine header -- but its `vt` can name an
 * interface pointer, and one of those reaching the guest untranslated is a
 * native vtable in guest hands.  So the two flat exports that legitimately
 * return a PROPVARIANT are served and then AUDITED: a string vector passes, an
 * IUnknown is cleared and refused.  (Every PROPVARIANT-bearing VTABLE slot is
 * refused outright at generation time; there is no post-hoc audit there,
 * because there is no wrapper to put one in.) */
HRESULT WINAPI __wine_mf_audit_propvariant_out( LONG *logged, const char *export_name,
                                                HRESULT hr, PROPVARIANT *pv )
{
    VARTYPE base;

    if (FAILED(hr) || !pv) return hr;
    base = pv->vt & VT_TYPEMASK;
    if (base != VT_UNKNOWN && base != VT_DISPATCH) return hr;

    if (!InterlockedExchange( logged, 1 ))
        FIXME( "mf: %s returned a PROPVARIANT of type 0x%04x, which carries an "
               "interface pointer.  There is no IID in the signature to say "
               "which of the %u rostered vtables the guest should get, so it "
               "is cleared rather than handed over raw.\n",
               export_name, pv->vt, MF_IFACE_COUNT );
    PropVariantClear( pv );
    return E_NOTIMPL;
}

/* ------------------------------------------------------- flat wrappers */
/* Each calls the real native export -- an ordinary internal call, since this
 * file is part of the same module -- and wraps/translates at the classified
 * positions.  The interception is spec2thunk's GUEST-IMPL redirect, so the
 * guest still imports the plain export name.
 *
 * The order below is the order of dlls/mfplat/mfplat.thunks' directives:
 * vending, translate-in, then the two refusal families. */

/* ---- vending: a statically typed out-interface ---- */

HRESULT WINAPI __wine_guest_MFCreateAttributes( IMFAttributes **attributes, UINT32 size )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateAttributes( attributes, size );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)attributes, MF_IFACE_IMFAttributes );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateMediaType( IMFMediaType **type )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateMediaType( type );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)type, MF_IFACE_IMFMediaType );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateSample( IMFSample **sample )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateSample( sample );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)sample, MF_IFACE_IMFSample );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateMemoryBuffer( DWORD max_length, IMFMediaBuffer **buffer )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateMemoryBuffer( max_length, buffer );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)buffer, MF_IFACE_IMFMediaBuffer );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateAlignedMemoryBuffer( DWORD max_length, DWORD alignment,
                                                         IMFMediaBuffer **buffer )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateAlignedMemoryBuffer( max_length, alignment, buffer );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)buffer, MF_IFACE_IMFMediaBuffer );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreate2DMediaBuffer( DWORD width, DWORD height, DWORD fourcc,
                                                   BOOL bottom_up, IMFMediaBuffer **buffer )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreate2DMediaBuffer( width, height, fourcc, bottom_up, buffer );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)buffer, MF_IFACE_IMFMediaBuffer );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateCollection( IMFCollection **collection )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateCollection( collection );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)collection, MF_IFACE_IMFCollection );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateEventQueue( IMFMediaEventQueue **queue )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateEventQueue( queue );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)queue, MF_IFACE_IMFMediaEventQueue );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateSourceResolver( IMFSourceResolver **resolver )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateSourceResolver( resolver );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)resolver, MF_IFACE_IMFSourceResolver );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateFile( MF_FILE_ACCESSMODE accessmode, MF_FILE_OPENMODE openmode,
                                          MF_FILE_FLAGS flags, LPCWSTR url,
                                          IMFByteStream **bytestream )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateFile( accessmode, openmode, flags, url, bytestream );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)bytestream, MF_IFACE_IMFByteStream );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateTempFile( MF_FILE_ACCESSMODE accessmode,
                                              MF_FILE_OPENMODE openmode, MF_FILE_FLAGS flags,
                                              IMFByteStream **bytestream )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateTempFile( accessmode, openmode, flags, bytestream );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)bytestream, MF_IFACE_IMFByteStream );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateSystemTimeSource( IMFPresentationTimeSource **time_source )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateSystemTimeSource( time_source );
    if (SUCCEEDED(hr))
        winecom_wrap_static( (void **)time_source, MF_IFACE_IMFPresentationTimeSource );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateTrackedSample( IMFTrackedSample **sample )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateTrackedSample( sample );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)sample, MF_IFACE_IMFTrackedSample );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateAudioMediaType( const WAVEFORMATEX *audioformat,
                                                    IMFAudioMediaType **mediatype )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateAudioMediaType( audioformat, mediatype );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)mediatype, MF_IFACE_IMFAudioMediaType );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateVideoMediaType( const MFVIDEOFORMAT *format,
                                                    IMFVideoMediaType **media_type )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateVideoMediaType( format, media_type );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)media_type, MF_IFACE_IMFVideoMediaType );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateVideoMediaTypeFromSubtype( const GUID *subtype,
                                                               IMFVideoMediaType **media_type )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateVideoMediaTypeFromSubtype( subtype, media_type );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)media_type, MF_IFACE_IMFVideoMediaType );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreateTransformActivate( IMFActivate **activate )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateTransformActivate( activate );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)activate, MF_IFACE_IMFActivate );
    return hr;
}

HRESULT WINAPI __wine_guest_MFGetPluginControl( IMFPluginControl **control )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFGetPluginControl( control );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)control, MF_IFACE_IMFPluginControl );
    return hr;
}

/* ---- an riid-typed out-interface ---- */

HRESULT WINAPI __wine_guest_MFCreateVideoSampleAllocatorEx( REFIID riid, void **allocator )
{
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    hr = MFCreateVideoSampleAllocatorEx( riid, allocator );
    return winecom_wrap_out_iface( hr, riid, allocator );
}

/* ---- translate-in, with or without a vended result ---- */

HRESULT WINAPI __wine_guest_MFCreateMediaBufferFromMediaType( IMFMediaType *media_type,
                                                              LONGLONG duration, DWORD min_length,
                                                              DWORD min_alignment,
                                                              IMFMediaBuffer **buffer )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!__wine_mf_translate_in( &logged, "MFCreateMediaBufferFromMediaType",
                                 "IMFMediaType", media_type, &host ))
    {
        winecom_refused_scrub_ptr( buffer );
        return E_NOTIMPL;
    }
    hr = MFCreateMediaBufferFromMediaType( host, duration, min_length, min_alignment, buffer );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)buffer, MF_IFACE_IMFMediaBuffer );
    return hr;
}

HRESULT WINAPI __wine_guest_MFWrapMediaType( IMFMediaType *original, REFGUID major,
                                             REFGUID subtype, IMFMediaType **wrapped )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!__wine_mf_translate_in( &logged, "MFWrapMediaType", "IMFMediaType",
                                 original, &host ))
    {
        winecom_refused_scrub_ptr( wrapped );
        return E_NOTIMPL;
    }
    hr = MFWrapMediaType( host, major, subtype, wrapped );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)wrapped, MF_IFACE_IMFMediaType );
    return hr;
}

HRESULT WINAPI __wine_guest_MFUnwrapMediaType( IMFMediaType *wrapped, IMFMediaType **original )
{
    static LONG logged;
    void *host;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!__wine_mf_translate_in( &logged, "MFUnwrapMediaType", "IMFMediaType",
                                 wrapped, &host ))
    {
        winecom_refused_scrub_ptr( original );
        return E_NOTIMPL;
    }
    hr = MFUnwrapMediaType( host, original );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)original, MF_IFACE_IMFMediaType );
    return hr;
}

/* An INPUT array of interfaces: every element is translated into a
 * stack/heap copy before the call, because the guest's array holds proxies
 * and the callee must see hosts.  The same shape libs/winecom's
 * WINECOM_CA_IFACE_ARR_IN has inside the dispatch loop. */
static HRESULT mf_translate_in_array( LONG *logged, const char *export_name,
                                      const char *iface_name, void **src, DWORD count,
                                      void ***out, void **stackbuf, DWORD stackn )
{
    void **dst;
    DWORD i;

    *out = NULL;
    if (!src || !count) return S_OK;
    if (count <= stackn) dst = stackbuf;
    else if (!(dst = malloc( count * sizeof(*dst) )))
        return E_OUTOFMEMORY;
    for (i = 0; i < count; i++)
    {
        if (!__wine_mf_translate_in( logged, export_name, iface_name, src[i], &dst[i] ))
        {
            if (dst != stackbuf) free( dst );
            return E_NOTIMPL;
        }
    }
    *out = dst;
    return S_OK;
}

HRESULT WINAPI __wine_guest_MFCreateStreamDescriptor( DWORD identifier, DWORD count,
                                                      IMFMediaType **types,
                                                      IMFStreamDescriptor **descriptor )
{
    static LONG logged;
    void *stackbuf[16], **hosts;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (FAILED(hr = mf_translate_in_array( &logged, "MFCreateStreamDescriptor",
                                           "IMFMediaType", (void **)types, count,
                                           &hosts, stackbuf, ARRAYSIZE(stackbuf) )))
    {
        winecom_refused_scrub_ptr( descriptor );
        return hr;
    }
    hr = MFCreateStreamDescriptor( identifier, count, (IMFMediaType **)hosts, descriptor );
    if (hosts && hosts != stackbuf) free( hosts );
    if (SUCCEEDED(hr))
        winecom_wrap_static( (void **)descriptor, MF_IFACE_IMFStreamDescriptor );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCreatePresentationDescriptor( DWORD count,
                                                            IMFStreamDescriptor **descriptors,
                                                            IMFPresentationDescriptor **out )
{
    static LONG logged;
    void *stackbuf[16], **hosts;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (FAILED(hr = mf_translate_in_array( &logged, "MFCreatePresentationDescriptor",
                                           "IMFStreamDescriptor", (void **)descriptors,
                                           count, &hosts, stackbuf, ARRAYSIZE(stackbuf) )))
    {
        winecom_refused_scrub_ptr( out );
        return hr;
    }
    hr = MFCreatePresentationDescriptor( count, (IMFStreamDescriptor **)hosts, out );
    if (hosts && hosts != stackbuf) free( hosts );
    if (SUCCEEDED(hr))
        winecom_wrap_static( (void **)out, MF_IFACE_IMFPresentationDescriptor );
    return hr;
}

BOOL WINAPI __wine_guest_MFCompareFullToPartialMediaType( IMFMediaType *full_type,
                                                          IMFMediaType *partial_type )
{
    static LONG logged;
    void *full, *partial;

    if (!mf_ready()) return FALSE;
    if (!__wine_mf_translate_in( &logged, "MFCompareFullToPartialMediaType",
                                 "IMFMediaType", full_type, &full ) ||
        !__wine_mf_translate_in( &logged, "MFCompareFullToPartialMediaType",
                                 "IMFMediaType", partial_type, &partial ))
        return FALSE;
    return MFCompareFullToPartialMediaType( full, partial );
}

/* One shape covers the ten "take an IMFMediaType and fill in plain data"
 * exports; writing them out is not repetition for its own sake, because each
 * has to name itself in the refusal. */
/* refusal hygiene by hand -- see hand_pv_in_mid: the refuse_scrub argument is
 * the statement that makes THIS export's refusal inert; the ones that take
 * only const inputs pass (void)0 and are audited as having no outs. */
#define MF_TRANSLATE_MEDIATYPE( name, decl_args, call_args, refuse_scrub )    \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        static LONG logged;                                                   \
        void *host;                                                           \
                                                                              \
        if (!mf_ready()) return E_FAIL;                                       \
        if (!__wine_mf_translate_in( &logged, #name, "IMFMediaType",          \
                                     media_type, &host ))                     \
        {                                                                     \
            refuse_scrub;                                                     \
            return E_NOTIMPL;                                                 \
        }                                                                     \
        return name call_args;                                                \
    }

MF_TRANSLATE_MEDIATYPE( MFCreateMFVideoFormatFromMFMediaType,
                        ( IMFMediaType *media_type, MFVIDEOFORMAT **format, UINT32 *size ),
                        ( host, format, size ),
                        ( winecom_refused_scrub_ptr( format ),
                          winecom_refused_scrub_dw( size ) ) )
MF_TRANSLATE_MEDIATYPE( MFCreateWaveFormatExFromMFMediaType,
                        ( IMFMediaType *media_type, WAVEFORMATEX **format, UINT32 *size,
                          UINT32 flags ),
                        ( host, format, size, flags ),
                        ( winecom_refused_scrub_ptr( format ),
                          winecom_refused_scrub_dw( size ) ) )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromWaveFormatEx,
                        ( IMFMediaType *media_type, const WAVEFORMATEX *format, UINT32 size ),
                        ( host, format, size ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromMFVideoFormat,
                        ( IMFMediaType *media_type, const MFVIDEOFORMAT *format, UINT32 size ),
                        ( host, format, size ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromAMMediaType,
                        ( IMFMediaType *media_type, const AM_MEDIA_TYPE *am_type ),
                        ( host, am_type ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromVideoInfoHeader,
                        ( IMFMediaType *media_type, const VIDEOINFOHEADER *vih, UINT32 size,
                          const GUID *subtype ),
                        ( host, vih, size, subtype ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromVideoInfoHeader2,
                        ( IMFMediaType *media_type, const VIDEOINFOHEADER2 *vih, UINT32 size,
                          const GUID *subtype ),
                        ( host, vih, size, subtype ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromMPEG1VideoInfo,
                        ( IMFMediaType *media_type, const MPEG1VIDEOINFO *vih, UINT32 size,
                          const GUID *subtype ),
                        ( host, vih, size, subtype ), (void)0 )
MF_TRANSLATE_MEDIATYPE( MFInitMediaTypeFromMPEG2VideoInfo,
                        ( IMFMediaType *media_type, const MPEG2VIDEOINFO *vih, UINT32 size,
                          const GUID *subtype ),
                        ( host, vih, size, subtype ), (void)0 )

#define MF_TRANSLATE_ATTRIBUTES( name, decl_args, call_args, refuse_scrub )   \
    HRESULT WINAPI __wine_guest_##name decl_args                              \
    {                                                                         \
        static LONG logged;                                                   \
        void *host;                                                           \
                                                                              \
        if (!mf_ready()) return E_FAIL;                                       \
        if (!__wine_mf_translate_in( &logged, #name, "IMFAttributes",         \
                                     attributes, &host ))                     \
        {                                                                     \
            refuse_scrub;                                                     \
            return E_NOTIMPL;                                                 \
        }                                                                     \
        return name call_args;                                                \
    }

/* MFGetAttributesAsBlob's buffer is left alone: the caller supplied it AND
 * its length, so an unwritten buffer tells it nothing it did not already
 * know -- the buffer-out exception the file banner names. */
MF_TRANSLATE_ATTRIBUTES( MFGetAttributesAsBlob,
                         ( IMFAttributes *attributes, UINT8 *buffer, UINT size ),
                         ( host, buffer, size ), (void)0 )
MF_TRANSLATE_ATTRIBUTES( MFGetAttributesAsBlobSize,
                         ( IMFAttributes *attributes, UINT32 *size ),
                         ( host, size ), winecom_refused_scrub_dw( size ) )
MF_TRANSLATE_ATTRIBUTES( MFInitAttributesFromBlob,
                         ( IMFAttributes *attributes, const UINT8 *buffer, UINT size ),
                         ( host, buffer, size ), (void)0 )

/* An IMFActivate the application registers as a local handler.  A game that
 * ships its own byte-stream handler hands a guest-implemented object here,
 * and that is the reverse-proxy gap; one it got back from MF (a transform
 * activate) unwraps and works. */
HRESULT WINAPI __wine_guest_MFRegisterLocalByteStreamHandler( const WCHAR *extension,
                                                              const WCHAR *mime,
                                                              IMFActivate *activate )
{
    static LONG logged;
    void *host;

    if (!mf_ready()) return E_FAIL;
    if (!__wine_mf_translate_in( &logged, "MFRegisterLocalByteStreamHandler",
                                 "IMFActivate", activate, &host ))
        return E_NOTIMPL;
    return MFRegisterLocalByteStreamHandler( extension, mime, host );
}

HRESULT WINAPI __wine_guest_MFRegisterLocalSchemeHandler( const WCHAR *scheme,
                                                          IMFActivate *activate )
{
    static LONG logged;
    void *host;

    if (!mf_ready()) return E_FAIL;
    if (!__wine_mf_translate_in( &logged, "MFRegisterLocalSchemeHandler",
                                 "IMFActivate", activate, &host ))
        return E_NOTIMPL;
    return MFRegisterLocalSchemeHandler( scheme, host );
}

/* ---- the async family: SERVED, through reverse proxies ---- */
/* This is the block the whole reverse-proxy exercise was for.  Every one of
 * these exports hands Media Foundation an object the APPLICATION implemented
 * and MF calls it back, usually from a work-queue thread the application never
 * created.  Each is written out rather than collapsed into a table because the
 * refusal path still names the export, and a guest developer reading a log
 * wants the name of the function they called.
 *
 * The shape is always the same three steps, and the middle one is an ordinary
 * internal call to Wine's own implementation:
 *
 *   translate every interface argument IN, with its roster type, so a
 *   guest-implemented object becomes a reverse proxy and one of ours unwraps;
 *   call the real export;
 *   give the borrows back, so an export that kept its own reference keeps the
 *   object and one that did not lets it go.
 *
 * MF_IN/MF_IN_END below are that bookkeeping and nothing else. */

#define MF_IN( name, iface_name, iface, obj, out )                            \
    __wine_mf_translate_in_iface( &logged, name, iface_name, iface, obj, out )

/* Seven of these exports are spec FORWARDS to rtworkq -- mfplat.dll publishes
 * the MF* name and rtworkq.dll implements it -- so there is no MF* symbol in
 * this module to make an internal call to, and the wrapper has to call the
 * Rtwq* function the forward points at.  Declared here rather than by
 * including rtworkq.h because that header names its own IRtwqAsyncCallback and
 * IRtwqAsyncResult types, which are the same objects under different C names;
 * void* says exactly what crosses and nothing more.  Each line is the forward
 * from dlls/mfplat/mfplat.spec, read left to right. */
HRESULT WINAPI RtwqCreateAsyncResult( void *object, void *callback, void *state,
                                      void **result );
HRESULT WINAPI RtwqPutWaitingWorkItem( HANDLE event, LONG priority, void *result,
                                       UINT64 *key );
HRESULT WINAPI RtwqScheduleWorkItem( void *result, INT64 timeout, UINT64 *key );
HRESULT WINAPI RtwqBeginRegisterWorkQueueWithMMCSS( DWORD queue, const WCHAR *usage_class,
                                                    DWORD taskid, LONG priority,
                                                    void *callback, void *state );
HRESULT WINAPI RtwqEndRegisterWorkQueueWithMMCSS( void *result, DWORD *taskid );
HRESULT WINAPI RtwqBeginUnregisterWorkQueueWithMMCSS( DWORD queue, void *callback,
                                                      void *state );
HRESULT WINAPI RtwqEndUnregisterWorkQueueWithMMCSS( void *result );

HRESULT WINAPI __wine_guest_MFCreateAsyncResult( IUnknown *object, IMFAsyncCallback *callback,
                                                 IUnknown *state, IMFAsyncResult **result )
{
    static LONG logged;
    void *obj = NULL, *cb = NULL, *st = NULL;
    HRESULT hr;

    /* refusal hygiene by hand -- see hand_pv_in_mid: this prologue scrub IS
     * the refusal scrub for every exit below, so it goes through the helper
     * and the lever can take it away. */
    winecom_refused_scrub_ptr( result );
    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFCreateAsyncResult", "IUnknown", MF_IFACE_IUnknown, object, &obj ) ||
        !MF_IN( "MFCreateAsyncResult", "IMFAsyncCallback", MF_IFACE_IMFAsyncCallback,
                callback, &cb ) ||
        !MF_IN( "MFCreateAsyncResult", "IUnknown (the state)", MF_IFACE_IUnknown,
                state, &st ))
        hr = E_NOTIMPL;
    else
    {
        hr = RtwqCreateAsyncResult( obj, cb, st, (void **)result );
        if (SUCCEEDED(hr)) winecom_wrap_static( (void **)result, MF_IFACE_IMFAsyncResult );
    }
    __wine_mf_translate_in_end( obj );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFInvokeCallback( IMFAsyncResult *result )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFInvokeCallback", "IMFAsyncResult", MF_IFACE_IMFAsyncResult,
                result, &res ))
        hr = E_NOTIMPL;
    else hr = MFInvokeCallback( res );
    __wine_mf_translate_in_end( res );
    return hr;
}

/* THE LOCAL MFT REGISTRY, and it is here because putting IClassFactory on the
 * roster made spec2thunk's flat audit able to SEE it.  Both of these take an
 * IClassFactory the application implements -- that is the whole point of a
 * "local" MFT: the process registers a transform of its own and Media
 * Foundation instantiates it through that factory.  Until IClassFactory was on
 * the roster the audit's "does this signature carry an interface" token set
 * did not contain the name, so both exports read as carrying no interface at
 * all and passed a guest proxy straight into native MF unclassified.  That is
 * the same hole ppc64le/mf/README.md records for a bare IUnknown * argument
 * and MFShutdownObject, found the same way and closed the same way.
 *
 * The factory is KEPT by the registry -- dlls/mfplat/main.c holds it in a list
 * and AddRefs it -- and MFTUnregisterLocal finds it again by POINTER IDENTITY.
 * That works across the boundary only because winecom_reverse_wrap interns:
 * the same guest object handed in twice yields the same native proxy, so the
 * pointer the registration stored is the pointer the unregistration presents.
 * A mechanism that minted a fresh proxy per call would register successfully
 * and then never be able to unregister, which is exactly the class of bug that
 * looks like it works.
 *
 * NULL is meaningful for MFTUnregisterLocal -- it means "unregister everything
 * this process registered" -- and winecom_to_native passes NULL through as
 * NULL, so it needs no special case here.
 */
HRESULT WINAPI __wine_guest_MFTRegisterLocal( IClassFactory *factory, REFGUID category,
                                              LPCWSTR name, UINT32 flags, UINT32 cinput,
                                              const MFT_REGISTER_TYPE_INFO *input_types,
                                              UINT32 coutput,
                                              const MFT_REGISTER_TYPE_INFO *output_types )
{
    static LONG logged;
    void *host = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFTRegisterLocal", "IClassFactory", MF_IFACE_IClassFactory,
                factory, &host ))
        return E_NOTIMPL;
    hr = MFTRegisterLocal( host, category, name, flags, cinput, input_types,
                           coutput, output_types );
    /* Give the borrow back: the registry took its own reference if it kept the
     * factory, and the interned proxy stays alive on that reference alone. */
    __wine_mf_translate_in_end( host );
    return hr;
}

HRESULT WINAPI __wine_guest_MFTUnregisterLocal( IClassFactory *factory )
{
    static LONG logged;
    void *host = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFTUnregisterLocal", "IClassFactory", MF_IFACE_IClassFactory,
                factory, &host ))
        return E_NOTIMPL;
    hr = MFTUnregisterLocal( host );
    __wine_mf_translate_in_end( host );
    return hr;
}

HRESULT WINAPI __wine_guest_MFPutWorkItem( DWORD queue, IMFAsyncCallback *callback,
                                           IUnknown *state )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFPutWorkItem", "IMFAsyncCallback", MF_IFACE_IMFAsyncCallback,
                callback, &cb ) ||
        !MF_IN( "MFPutWorkItem", "IUnknown (the state)", MF_IFACE_IUnknown, state, &st ))
        hr = E_NOTIMPL;
    else hr = MFPutWorkItem( queue, cb, st );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFPutWorkItem2( DWORD queue, LONG priority,
                                            IMFAsyncCallback *callback, IUnknown *state )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFPutWorkItem2", "IMFAsyncCallback", MF_IFACE_IMFAsyncCallback,
                callback, &cb ) ||
        !MF_IN( "MFPutWorkItem2", "IUnknown (the state)", MF_IFACE_IUnknown, state, &st ))
        hr = E_NOTIMPL;
    else hr = MFPutWorkItem2( queue, priority, cb, st );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFPutWorkItemEx( DWORD queue, IMFAsyncResult *result )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFPutWorkItemEx", "IMFAsyncResult", MF_IFACE_IMFAsyncResult, result, &res ))
        hr = E_NOTIMPL;
    else hr = MFPutWorkItemEx( queue, res );
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFPutWorkItemEx2( DWORD queue, LONG priority,
                                              IMFAsyncResult *result )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFPutWorkItemEx2", "IMFAsyncResult", MF_IFACE_IMFAsyncResult, result, &res ))
        hr = E_NOTIMPL;
    else hr = MFPutWorkItemEx2( queue, priority, res );
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFPutWaitingWorkItem( HANDLE event, LONG priority,
                                                  IMFAsyncResult *result,
                                                  MFWORKITEM_KEY *key )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!MF_IN( "MFPutWaitingWorkItem", "IMFAsyncResult", MF_IFACE_IMFAsyncResult,
                result, &res ))
    {
        winecom_refused_scrub_mem( key, sizeof(*key) );
        hr = E_NOTIMPL;
    }
    else hr = RtwqPutWaitingWorkItem( event, priority, res, (UINT64 *)key );
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFScheduleWorkItem( IMFAsyncCallback *callback, IUnknown *state,
                                                INT64 timeout, MFWORKITEM_KEY *key )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFScheduleWorkItem", "IMFAsyncCallback", MF_IFACE_IMFAsyncCallback,
                callback, &cb ) ||
    /* refusal hygiene by hand -- see hand_pv_in_mid */
        !MF_IN( "MFScheduleWorkItem", "IUnknown (the state)", MF_IFACE_IUnknown,
                state, &st ))
    {
        winecom_refused_scrub_mem( key, sizeof(*key) );
        hr = E_NOTIMPL;
    }
    else hr = MFScheduleWorkItem( cb, st, timeout, key );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFScheduleWorkItemEx( IMFAsyncResult *result, INT64 timeout,
                                                  MFWORKITEM_KEY *key )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!MF_IN( "MFScheduleWorkItemEx", "IMFAsyncResult", MF_IFACE_IMFAsyncResult,
                result, &res ))
    {
        winecom_refused_scrub_mem( key, sizeof(*key) );
        hr = E_NOTIMPL;
    }
    else hr = RtwqScheduleWorkItem( res, timeout, (UINT64 *)key );
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFBeginCreateFile( MF_FILE_ACCESSMODE access_mode,
                                               MF_FILE_OPENMODE open_mode,
                                               MF_FILE_FLAGS flags, const WCHAR *path,
                                               IMFAsyncCallback *callback, IUnknown *state,
                                               IUnknown **cancel_cookie )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFBeginCreateFile", "IMFAsyncCallback", MF_IFACE_IMFAsyncCallback,
                callback, &cb ) ||
    /* refusal hygiene by hand -- see hand_pv_in_mid */
        !MF_IN( "MFBeginCreateFile", "IUnknown (the state)", MF_IFACE_IUnknown,
                state, &st ))
    {
        winecom_refused_scrub_ptr( cancel_cookie );
        hr = E_NOTIMPL;
    }
    else
    {
        hr = MFBeginCreateFile( access_mode, open_mode, flags, path, cb, st, cancel_cookie );
        /* The cancel cookie is a NATIVE object the caller passes back to
         * MFCancelCreateFile, so it crosses as a proxy like any other vended
         * interface rather than as an opaque token. */
        if (SUCCEEDED(hr)) winecom_wrap_static( (void **)cancel_cookie, MF_IFACE_IUnknown );
    }
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFEndCreateFile( IMFAsyncResult *result, IMFByteStream **stream )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!MF_IN( "MFEndCreateFile", "IMFAsyncResult", MF_IFACE_IMFAsyncResult, result, &res ))
    {
        winecom_refused_scrub_ptr( stream );
        hr = E_NOTIMPL;
    }
    else
    {
        hr = MFEndCreateFile( res, stream );
        if (SUCCEEDED(hr)) winecom_wrap_static( (void **)stream, MF_IFACE_IMFByteStream );
    }
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFCancelCreateFile( IUnknown *cancel_cookie )
{
    static LONG logged;
    void *cookie = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFCancelCreateFile", "IUnknown (the cancel cookie)", MF_IFACE_IUnknown,
                cancel_cookie, &cookie ))
        hr = E_NOTIMPL;
    else hr = MFCancelCreateFile( cookie );
    __wine_mf_translate_in_end( cookie );
    return hr;
}

HRESULT WINAPI __wine_guest_MFBeginRegisterWorkQueueWithMMCSS( DWORD queue,
                                                               const WCHAR *usage_class,
                                                               DWORD taskid,
                                                               IMFAsyncCallback *callback,
                                                               IUnknown *state )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFBeginRegisterWorkQueueWithMMCSS", "IMFAsyncCallback",
                MF_IFACE_IMFAsyncCallback, callback, &cb ) ||
        !MF_IN( "MFBeginRegisterWorkQueueWithMMCSS", "IUnknown (the state)",
                MF_IFACE_IUnknown, state, &st ))
        hr = E_NOTIMPL;
    else hr = MFBeginRegisterWorkQueueWithMMCSS( queue, usage_class, taskid, cb, st );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFBeginRegisterWorkQueueWithMMCSSEx( DWORD queue,
                                                                 const WCHAR *usage_class,
                                                                 DWORD taskid, LONG priority,
                                                                 IMFAsyncCallback *callback,
                                                                 IUnknown *state )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFBeginRegisterWorkQueueWithMMCSSEx", "IMFAsyncCallback",
                MF_IFACE_IMFAsyncCallback, callback, &cb ) ||
        !MF_IN( "MFBeginRegisterWorkQueueWithMMCSSEx", "IUnknown (the state)",
                MF_IFACE_IUnknown, state, &st ))
        hr = E_NOTIMPL;
    else hr = RtwqBeginRegisterWorkQueueWithMMCSS( queue, usage_class, taskid,
                                                   priority, cb, st );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFEndRegisterWorkQueueWithMMCSS( IMFAsyncResult *result,
                                                             DWORD *taskid )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!MF_IN( "MFEndRegisterWorkQueueWithMMCSS", "IMFAsyncResult",
                MF_IFACE_IMFAsyncResult, result, &res ))
    {
        winecom_refused_scrub_dw( taskid );
        hr = E_NOTIMPL;
    }
    else hr = RtwqEndRegisterWorkQueueWithMMCSS( res, taskid );
    __wine_mf_translate_in_end( res );
    return hr;
}

HRESULT WINAPI __wine_guest_MFBeginUnregisterWorkQueueWithMMCSS( DWORD queue,
                                                                 IMFAsyncCallback *callback,
                                                                 IUnknown *state )
{
    static LONG logged;
    void *cb = NULL, *st = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFBeginUnregisterWorkQueueWithMMCSS", "IMFAsyncCallback",
                MF_IFACE_IMFAsyncCallback, callback, &cb ) ||
        !MF_IN( "MFBeginUnregisterWorkQueueWithMMCSS", "IUnknown (the state)",
                MF_IFACE_IUnknown, state, &st ))
        hr = E_NOTIMPL;
    else hr = RtwqBeginUnregisterWorkQueueWithMMCSS( queue, cb, st );
    __wine_mf_translate_in_end( cb );
    __wine_mf_translate_in_end( st );
    return hr;
}

HRESULT WINAPI __wine_guest_MFEndUnregisterWorkQueueWithMMCSS( IMFAsyncResult *result )
{
    static LONG logged;
    void *res = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "MFEndUnregisterWorkQueueWithMMCSS", "IMFAsyncResult",
                MF_IFACE_IMFAsyncResult, result, &res ))
        hr = E_NOTIMPL;
    else hr = RtwqEndUnregisterWorkQueueWithMMCSS( res );
    __wine_mf_translate_in_end( res );
    return hr;
}

/* MFAddPeriodicCallback's first argument is a BARE FUNCTION POINTER the
 * application supplies, not an interface -- the reverse-proxy gap in its
 * oldest form, and the one form a reverse proxy does NOT close.  The port
 * intercepts a guest function pointer at REGISTRATION (dlls/ntdll's trampoline
 * pool), which needs spec2thunk to be told this export has a callback slot.
 * Refused with the callback type named, rather than letting native code
 * `bctrl` into x86-64 bytes. */
HRESULT WINAPI __wine_guest_MFAddPeriodicCallback( MFPERIODICCALLBACK callback,
                                                   IUnknown *context, DWORD *key )
{
    static LONG logged;
    void *host = NULL;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_dw( key );
    if (!MF_IN( "MFAddPeriodicCallback", "IUnknown (the callback context)",
                MF_IFACE_IUnknown, context, &host ))
        return E_NOTIMPL;
    __wine_mf_translate_in_end( host );
    if (!InterlockedExchange( &logged, 1 ))
        FIXME( "mf: refusing MFAddPeriodicCallback(%p): its first argument is a "
               "bare MFPERIODICCALLBACK function pointer in the GUEST's image, "
               "and native MF would call it directly.  A reverse proxy does not "
               "help -- there is no vtable -- and guest function pointers are "
               "intercepted at registration by the trampoline pool, which this "
               "export has no callback slot declared to spec2thunk for.\n",
               callback );
    return E_NOTIMPL;
}


/* ------------------------------------------------- the reverse-proxy hook */
/* See include/wine/winecom_selftest.h for why a test hook lives in a shipping
 * module.  Nothing in Wine calls this; ppc64le/winecom/check-reverse-proxy.sh
 * is its only caller, and it is entered through the ordinary spec2thunk
 * GUEST-IMPL path so that what it measures is the real boundary. */

/* Attribute keys of this file's own invention.  A GUID is a GUID: the guest
 * object records what it was told under each of them and the values are
 * checked on both sides. */
static const GUID st_key_uint32 =
    { 0x5e7e0001, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x01 } };
static const GUID st_key_uint64 =
    { 0x5e7e0002, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x02 } };
static const GUID st_key_double =
    { 0x5e7e0003, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x03 } };
static const GUID st_key_string =
    { 0x5e7e0004, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x04 } };
static const GUID st_key_unknown =
    { 0x5e7e0005, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x05 } };
static const GUID st_key_item =
    { 0x5e7e0006, 0x9a4d, 0x4f0b, { 0xb1, 0x22, 0x00, 0x11, 0x22, 0x33, 0x44, 0x06 } };

static void st_check( struct winecom_selftest_report *r, BOOL ok, const char *what )
{
    r->checks++;
    if (ok) return;
    r->failures++;
    if (!r->first_fail) r->first_fail = r->checks;
    ERR( "winecom selftest: check %u FAILED: %s\n", r->checks, what );
}

/* The foreign-thread leg.  A thread NATIVE code created, which has never run
 * guest code: entering the guest from here exercises the emulator's lazy
 * per-thread initialisation, which is the thing an XAudio2 mixer thread or a
 * Media Foundation work queue relies on without ever asking for it.
 *
 * That it works at all is a property of unix_emu_run_entry, which adopts a
 * bridge handle for whatever thread calls it and sets that thread's GS base --
 * so there is no thread-specific code anywhere in this layer.  Measured here
 * rather than assumed, because "no code was needed" and "it happens to work"
 * look identical until something checks. */
struct st_foreign
{
    IMFAttributes *attributes;
    UINT64 ns;
    BOOL ok;
};

static DWORD WINAPI st_foreign_thread( void *arg )
{
    struct st_foreign *f = arg;
    LARGE_INTEGER freq, a, b;
    HRESULT hr;

    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &a );
    /* DeleteAllItems rather than DeleteItem: the guest's DeleteAllItems is
     * the leg that makes THUNKED CALLS from inside the reverse call, which is
     * the pattern a streaming audio callback has and the one that has to work
     * on a thread native code created. */
    hr = IMFAttributes_DeleteAllItems( f->attributes );
    QueryPerformanceCounter( &b );
    f->ns = freq.QuadPart ? ((UINT64)(b.QuadPart - a.QuadPart) * 1000000000ull)
                            / (UINT64)freq.QuadPart : 0;
    /* The guest's DeleteItem answers E_NOTIMPL by design; what is being
     * checked is that the call ARRIVED and came back, not what it said. */
    f->ok = (hr == E_NOTIMPL);
    return 0;
}

static UINT st_time( IMFAttributes *obj, UINT reps )
{
    LARGE_INTEGER freq, a, b;
    UINT64 ns;
    UINT i;

    QueryPerformanceFrequency( &freq );
    if (!freq.QuadPart) return 0;
    for (i = 0; i < 16; i++) IMFAttributes_DeleteItem( obj, &st_key_item );  /* warm */
    QueryPerformanceCounter( &a );
    for (i = 0; i < reps; i++) IMFAttributes_DeleteItem( obj, &st_key_item );
    QueryPerformanceCounter( &b );
    ns = ((UINT64)(b.QuadPart - a.QuadPart) * 1000000000ull) / (UINT64)freq.QuadPart;
    return (UINT)(ns / reps);
}

static void st_time_crossings( IMFAttributes *guest_obj, IMFAttributes *native_obj,
                               struct winecom_selftest_report *report )
{
    struct st_foreign f = { guest_obj, 0, FALSE };
    HANDLE thread;

    report->ns_native  = st_time( native_obj, WINECOM_ST_TIMED_CALLS );
    report->ns_reverse = st_time( guest_obj,  WINECOM_ST_TIMED_CALLS );

    if ((thread = CreateThread( NULL, 0, st_foreign_thread, &f, 0, NULL )))
    {
        WaitForSingleObject( thread, 10000 );
        CloseHandle( thread );
        report->ns_foreign = (UINT)f.ns;
        report->foreign_ok = f.ok;
    }
    else ERR( "winecom selftest: could not create the foreign-thread leg\n" );

    ERR( "winecom selftest: one call costs %u ns to a NATIVE object, %u ns to "
         "a GUEST object through a reverse proxy (%u.%02ux), and the FIRST one "
         "on a thread that had never run guest code cost %u ns%s\n",
         report->ns_native, report->ns_reverse,
         report->ns_native ? report->ns_reverse / report->ns_native : 0,
         report->ns_native ? (report->ns_reverse * 100 / report->ns_native) % 100 : 0,
         report->ns_foreign, report->foreign_ok ? "" : " (AND DID NOT ARRIVE)" );
}

HRESULT WINAPI __wine_winecom_reverse_selftest( IMFAttributes *attributes,
                                                IMFSimpleAudioVolume *volume,
                                                struct winecom_selftest_report *report )
{
    IMFAttributes *native_obj = NULL;
    IUnknown *back = NULL, *qi = NULL;
    PROPVARIANT pv;
    UINT32 count = 0;
    ULONG refs0, refs1;
    HRESULT hr;

    if (!attributes || !volume || !report) return E_POINTER;
    memset( report, 0, sizeof(*report) );
    if (!mf_ready()) return E_FAIL;

    /* A real native object for the interface-argument class, so the guest is
     * handed something that genuinely has a ppc64 vtable and must arrive as a
     * FORWARD proxy -- a proxy minted INSIDE a reverse call, which is the one
     * place the two directions meet. */
    if (FAILED(hr = MFCreateAttributes( &native_obj, 4 ))) return hr;

    /* ---- integers ---- */
    hr = IMFAttributes_SetUINT32( attributes, &st_key_uint32, WINECOM_ST_UINT32 );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetUINT32 return value" );

    hr = IMFAttributes_SetUINT64( attributes, &st_key_uint64, WINECOM_ST_UINT64 );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetUINT64 return value" );

    /* ---- a by-value double: WINECOM_F_REV, refused forward, served here ---- */
    hr = IMFAttributes_SetDouble( attributes, &st_key_double, WINECOM_ST_DOUBLE );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetDouble return value" );

    /* ---- a by-value single-precision float, the other FP width ---- */
    hr = IMFSimpleAudioVolume_SetMasterVolume( volume, WINECOM_ST_FLOAT );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetMasterVolume return value" );

    /* ---- a string ---- */
    hr = IMFAttributes_SetString( attributes, &st_key_string, WINECOM_ST_STRING );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetString return value" );

    /* ---- an interface IN ---- */
    hr = IMFAttributes_SetUnknown( attributes, &st_key_unknown, (IUnknown *)native_obj );
    report->calls++;
    st_check( report, hr == WINECOM_ST_HR_OK, "SetUnknown return value" );

    /* ---- an interface OUT, through a REFIID.  The guest hands back the very
     * pointer it was given one call ago, and it must arrive here as the
     * ORIGINAL native object -- not as a wrapper around a wrapper. ---- */
    hr = IMFAttributes_GetUnknown( attributes, &st_key_unknown, &IID_IUnknown,
                                   (void **)&back );
    report->calls++;
    st_check( report, SUCCEEDED(hr), "GetUnknown return value" );
    st_check( report, back == (IUnknown *)native_obj,
              "GetUnknown round trip: the native object the guest was handed "
              "did not come back as itself" );
    report->roundtrip_ok = (back == (IUnknown *)native_obj);
    if (back) IUnknown_Release( back );

    /* ---- an integer OUT ---- */
    hr = IMFAttributes_GetCount( attributes, &count );
    report->calls++;
    st_check( report, SUCCEEDED(hr), "GetCount return value" );
    report->guest_count = count;

    /* ---- THE REFUSAL DISCIPLINE.  SetItem takes a PROPVARIANT, which the
     * tables refuse for what it IS rather than for which way it travels, so
     * the reverse direction must refuse it too and the guest must never see
     * it. ---- */
    memset( &pv, 0, sizeof(pv) );
    pv.vt = VT_UI4;
    pv.ulVal = 7;
    hr = IMFAttributes_SetItem( attributes, &st_key_item, &pv );
    st_check( report, hr == E_NOTIMPL,
              "SetItem must still be refused in the reverse direction" );
    report->refuse_ok = (hr == E_NOTIMPL);

    /* ---- IDENTITY.  QueryInterface goes to the guest, which answers with its
     * own pointer; interning must turn that back into THIS proxy, or a COM
     * caller comparing two pointers for identity gets the wrong answer. ---- */
    hr = IMFAttributes_QueryInterface( attributes, &IID_IMFAttributes, (void **)&qi );
    report->calls++;
    st_check( report, SUCCEEDED(hr) && (void *)qi == (void *)attributes,
              "QueryInterface identity: the proxy did not come back as itself" );
    report->identity_ok = (SUCCEEDED(hr) && (void *)qi == (void *)attributes);
    if (qi) IUnknown_Release( qi );

    /* ---- REFERENCE BALANCE.  AddRef/Release on the proxy is served by the
     * runtime and must be exactly symmetric; if it is not, a sink outlives its
     * registration or dies inside it. ---- */
    refs0 = IMFAttributes_AddRef( attributes );
    refs1 = IMFAttributes_Release( attributes );
    st_check( report, refs0 == refs1 + 1,
              "AddRef/Release on the reverse proxy is not symmetric" );
    report->refs_leaked = (refs0 != refs1 + 1);

    /* ---- WHAT ONE CROSSING COSTS.  Timed LAST, so nothing above has to
     * tolerate a method being called two thousand times, and on DeleteItem,
     * which no check above looks at.  DeleteItem is the right shape for this:
     * one integer-class argument, no interface translation, no allocation --
     * so what is measured is the crossing and not the marshalling of an
     * unusual signature. ---- */
    st_time_crossings( attributes, native_obj, report );

    IMFAttributes_Release( native_obj );
    return report->failures ? E_FAIL : S_OK;
}

/* One level of the nest.  Native code calls a method on the guest object; the
 * guest's implementation of that method calls this export again.  There is
 * nothing else to it -- the whole content is the round trip. */
HRESULT WINAPI __wine_winecom_reverse_nest( IMFAttributes *attributes,
                                            UINT *depth_reached )
{
    (void)depth_reached;   /* the GUEST owns it; it crosses as plain memory */
    if (!attributes) return E_POINTER;
    return IMFAttributes_LockStore( attributes );
}

HRESULT WINAPI __wine_guest___wine_winecom_reverse_nest( IMFAttributes *attributes,
                                                         UINT *depth_reached )
{
    static LONG logged;
    void *attr = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    /* refusal hygiene by hand -- see hand_pv_in_mid */
    if (!MF_IN( "__wine_winecom_reverse_nest", "IMFAttributes",
                MF_IFACE_IMFAttributes, attributes, &attr ))
    {
        winecom_refused_scrub_dw( depth_reached );
        hr = E_NOTIMPL;
    }
    else hr = __wine_winecom_reverse_nest( attr, depth_reached );
    __wine_mf_translate_in_end( attr );
    return hr;
}

/* The guest-lane wrapper: the two objects arrive as GUEST pointers and become
 * reverse proxies here, borrowed for the duration of the call.  This is the
 * ONLY difference between what the gate's guest leg measures and what a native
 * caller of the hook would measure, and it is the difference the gate is for. */
HRESULT WINAPI __wine_guest___wine_winecom_reverse_selftest(
        IMFAttributes *attributes, IMFSimpleAudioVolume *volume,
        struct winecom_selftest_report *report )
{
    static LONG logged;
    void *attr = NULL, *vol = NULL;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (!MF_IN( "__wine_winecom_reverse_selftest", "IMFAttributes",
                MF_IFACE_IMFAttributes, attributes, &attr ) ||
    /* refusal hygiene by hand -- see hand_pv_in_mid */
        !MF_IN( "__wine_winecom_reverse_selftest", "IMFSimpleAudioVolume",
                MF_IFACE_IMFSimpleAudioVolume, volume, &vol ))
    {
        winecom_refused_scrub_mem( report, sizeof(*report) );
        hr = E_NOTIMPL;
    }
    else hr = __wine_winecom_reverse_selftest( attr, vol, report );
    __wine_mf_translate_in_end( attr );
    __wine_mf_translate_in_end( vol );
    if (FAILED(hr) && !InterlockedExchange( &logged, 1 ))
        ERR( "winecom selftest: the hook reported failure (%#lx)\n", (long)hr );
    return hr;
}

/* ---- the cross-surface family ---- */

HRESULT WINAPI __wine_guest_MFCreateMFByteStreamOnStream( IStream *stream,
                                                          IMFByteStream **bytestream )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( bytestream );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateMFByteStreamOnStream",
                                           "IStream", "system-COM (combase)" );
}

HRESULT WINAPI __wine_guest_MFCreateMFByteStreamOnStreamEx( IUnknown *stream,
                                                            IMFByteStream **bytestream )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( bytestream );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateMFByteStreamOnStreamEx",
                                           "IUnknown (an IStream or IRandomAccessStream)",
                                           "system-COM (combase)" );
}

HRESULT WINAPI __wine_guest_MFSerializeAttributesToStream( IMFAttributes *attributes,
                                                           DWORD flags, IStream *stream )
{
    static LONG logged;

    return __wine_mf_refuse_cross_surface( &logged, "MFSerializeAttributesToStream",
                                           "IStream", "system-COM (combase)" );
}

HRESULT WINAPI __wine_guest_MFDeserializeAttributesFromStream( IMFAttributes *attributes,
                                                               DWORD flags, IStream *stream )
{
    static LONG logged;

    return __wine_mf_refuse_cross_surface( &logged, "MFDeserializeAttributesFromStream",
                                           "IStream", "system-COM (combase)" );
}

HRESULT WINAPI __wine_guest_MFCreateDXGIDeviceManager( UINT *token,
                                                       IMFDXGIDeviceManager **manager )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( manager );
    winecom_refused_scrub_dw( token );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateDXGIDeviceManager",
                                           "IMFDXGIDeviceManager (which takes an "
                                           "ID3D11Device)", "d3d11" );
}

HRESULT WINAPI __wine_guest_MFLockDXGIDeviceManager( UINT *token,
                                                     IMFDXGIDeviceManager **manager )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( manager );
    winecom_refused_scrub_dw( token );
    return __wine_mf_refuse_cross_surface( &logged, "MFLockDXGIDeviceManager",
                                           "IMFDXGIDeviceManager (which takes an "
                                           "ID3D11Device)", "d3d11" );
}

HRESULT WINAPI __wine_guest_MFCreateDXGISurfaceBuffer( REFIID riid, IUnknown *surface,
                                                       UINT subresource, BOOL bottomup,
                                                       IMFMediaBuffer **buffer )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( buffer );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateDXGISurfaceBuffer",
                                           "ID3D11Texture2D", "d3d11" );
}

HRESULT WINAPI __wine_guest_MFCreateDXSurfaceBuffer( REFIID riid, IUnknown *surface,
                                                     BOOL bottom_up, IMFMediaBuffer **buffer )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( buffer );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateDXSurfaceBuffer",
                                           "IDirect3DSurface9", "d3d9" );
}

HRESULT WINAPI __wine_guest_MFCreateLegacyMediaBufferOnMFMediaBuffer( IMFSample *sample,
                                                                      IMFMediaBuffer *media_buffer,
                                                                      DWORD offset,
                                                                      IMediaBuffer **obj )
{
    static LONG logged;

    /* refusal hygiene by hand -- see hand_pv_in_mid */
    winecom_refused_scrub_ptr( obj );
    return __wine_mf_refuse_cross_surface( &logged, "MFCreateLegacyMediaBufferOnMFMediaBuffer",
                                           "IMediaBuffer (a DirectShow/DMO interface)",
                                           "no rostered" );
}

/* ---- PROPVARIANT: served, then audited ---- */

HRESULT WINAPI __wine_guest_MFCreateMediaEvent( MediaEventType type, REFGUID extended_type,
                                                HRESULT status, const PROPVARIANT *value,
                                                IMFMediaEvent **event )
{
    static LONG logged;
    HRESULT hr;

    if (!mf_ready()) return E_FAIL;
    if (value && ((value->vt & VT_TYPEMASK) == VT_UNKNOWN ||
                  (value->vt & VT_TYPEMASK) == VT_DISPATCH))
    {
        if (!InterlockedExchange( &logged, 1 ))
            FIXME( "mf: MFCreateMediaEvent was given a PROPVARIANT of type 0x%04x, "
                   "which carries an interface pointer the guest owns; there is no "
                   "IID in the signature to translate it by.  Refusing.\n", value->vt );
        /* refusal hygiene by hand -- see hand_pv_in_mid */
        winecom_refused_scrub_ptr( event );
        return E_NOTIMPL;
    }
    hr = MFCreateMediaEvent( type, extended_type, status, value, event );
    if (SUCCEEDED(hr)) winecom_wrap_static( (void **)event, MF_IFACE_IMFMediaEvent );
    return hr;
}
