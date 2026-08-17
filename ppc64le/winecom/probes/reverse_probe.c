/*
 * reverse_probe -- the REVERSE-PROXY mechanism, measured.
 *
 * GUEST ONLY, and unavoidably so: what it measures is native ppc64 code
 * calling an x86-64 vtable, which is a thing that does not exist in a native
 * run.  There is no second build of this file to compare against, so every
 * claim it makes is a value check against a constant both sides compile from
 * include/wine/winecom_selftest.h.
 *
 * WHAT IT IS.  The program builds two COM objects IN ITS OWN IMAGE -- an
 * IMFAttributes and an IMFSimpleAudioVolume, x86-64 vtables at guest addresses
 * -- and hands them to __wine_winecom_reverse_selftest, a native hook that
 * calls one method of every argument class libs/winecom/reverse.c marshals:
 *
 *      SetUINT32          an integer
 *      SetUINT64          a wide integer
 *      SetDouble          a BY-VALUE DOUBLE, which the forward direction
 *                         refuses (its invoker has integer registers only) and
 *                         the reverse direction serves from the same row
 *      SetMasterVolume    a BY-VALUE FLOAT, the other floating-point width
 *      SetString          a WCHAR pointer
 *      SetUnknown         an INTERFACE IN -- a real native object, which
 *                         arrives here as a FORWARD proxy minted inside a
 *                         reverse call
 *      GetUnknown         an INTERFACE OUT through a REFIID
 *      GetCount           an integer OUT
 *      SetItem            a PROPVARIANT, which the tables refuse for what it
 *                         IS rather than for which way it travels, and which
 *                         must therefore STILL be refused
 *      QueryInterface     identity
 *      AddRef/Release     reference balance
 *
 * BOTH SIDES CHECK.  The native hook checks what came back to it (return
 * values, the round trip, identity, the refusal) and reports it in a struct;
 * this program checks what arrived here (every argument value, byte for byte
 * and bit for bit) and then checks the hook's report.  A mechanism that
 * carried the call but corrupted an argument would pass either half alone.
 *
 * THE ROUND TRIP, in both directions, is the check that is easiest to pass by
 * accident and hardest to pass correctly:
 *
 *   * native -> guest -> native.  SetUnknown hands this program a native
 *     object; GetUnknown hands it straight back; the hook requires the pointer
 *     it gets to be the pointer it gave.  A mechanism that minted a fresh
 *     wrapper each way would return something else that worked just as well
 *     until a caller compared two pointers.
 *   * guest -> native -> guest.  This program asks the forward proxy it was
 *     given for IID_IUnknown and requires the SAME POINTER back, which is the
 *     interning claim on this side.
 *
 * NO C RUNTIME (-DREVERSE_PROBE_NO_CRT): the program formats its own output
 * and writes it with WriteFile.  A CRT would add a second variable to a test
 * whose whole value is that only one thing is under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#define INITGUID

/* The MF attribute keys are spelled EXTERN_GUID, which declares without
 * defining even under INITGUID.  Wine's own libs/mfuuid does this
 * redefinition for the same reason; here it is what lets the guest build link
 * with no import library but the ones this probe's own .def describes. */
#undef EXTERN_GUID
#define EXTERN_GUID DEFINE_GUID

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>

#include <wine/winecom_selftest.h>

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

/* An OBSERVATION rather than a step: a duration, which is a different number
 * every run.  It goes to stderr, so the transcript this gate compares against
 * itself (traced run vs untraced) stays byte-stable -- the same rule
 * ppc64le/audio's `note:` lines keep, for the same reason. */
static void note( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_ERROR_HANDLE ), s, n, &written, NULL );
}

static void note_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    note( buf + i );
}

static void out_hex( ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out( buf );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    out( buf + i );
}

/* IsEqualGUID is memcmp, and there is no CRT here.  Comparing the fields is
 * also the honest thing: a GUID is a struct, not sixteen bytes that happen to
 * be adjacent. */
static BOOL guid_eq( const GUID *a, const GUID *b )
{
    int i;

    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return FALSE;
    for (i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return FALSE;
    return TRUE;
}

static int checks, failures;

static void step( const char *what, int ok, const char *detail )
{
    checks++;
    if (!ok) failures++;
    out( "step " );
    out_dec( checks );
    out( " " );
    out( what );
    out( ok ? ": ok" : ": FAIL" );
    if (detail) { out( " ("); out( detail ); out( ")" ); }
    out( "\n" );
}

/* ------------------------------------------------- the guest COM objects */

/* What the hook told us.  Recorded rather than checked on the spot, because a
 * method that was never called and a method called with the wrong value have
 * to be told apart, and "never called" is only visible once the whole run is
 * over. */
static struct
{
    ULONG   entered[40];        /* per-slot entry count, by method index */
    UINT32  uint32;
    UINT64  uint64;
    double  dbl;
    float   flt;
    WCHAR   str[64];
    IUnknown *unknown;          /* the forward proxy SetUnknown handed us */
    IUnknown *unknown_qi;       /* ...and what IT answered for IID_IUnknown */
    HRESULT unknown_qi_hr;
    ULONG   qi_count;
    ULONG   peak_refs;

    /* THE PATTERN DOOM DIED ON.  A reverse-called guest method that itself
     * makes an ordinary thunked call back into native code -- which is not an
     * exotic case at all, it is THE streaming-audio shape: OnBufferEnd fires
     * and the callback immediately calls SubmitSourceBuffer.  The reverse call
     * is a NESTED emulator run, and the trap the guest's thunk stub raises has
     * to be consumed by that nested run's trap dispatch exactly as the
     * outermost run consumes one.
     *
     * Recorded per THREAD KIND, because the two are different claims: the
     * guest's own thread has run guest code since RtlUserThreadStart, while a
     * thread NATIVE code created has whatever the emulator lazily built for it
     * at its first crossing -- and an audio mixer thread is always the second
     * kind. */
    ULONG   tid_main, tid_foreign;
    ULONG   thunk_int_ok,         thunk_iface_ok;
    ULONG   foreign_ran, foreign_thunk_int_ok, foreign_thunk_iface_ok;
} seen;

#define MARK( n )  (seen.entered[(n)]++)

/* AN ORDINARY FLAT THUNK CALL, made from inside a reverse-called guest
 * method.  GetCurrentThreadId and GetTickCount are guest kernel32 stubs: each
 * traps, ntdll maps the RIP to the native export, and the value comes back.
 * Nothing about them is special -- which is the point, because "the guest can
 * still call an API from inside a callback" is the thing that has to be true
 * for any callback-driven API to work at all. */
static ULONG thunk_call_int( void )
{
    DWORD tid = GetCurrentThreadId();
    DWORD t0  = GetTickCount();

    return (tid != 0 && t0 != 0) ? 1 : 0;
}

/* AND ONE THAT CARRIES AN INTERFACE.  MFCreateAttributes is a flat export
 * whose wrapper VENDS an interface -- so the guest gets a forward proxy minted
 * inside a reverse call -- and GetCount on it is a COM vtable trap from the
 * same place.  Three crossings deep: guest -> native (the hook) -> guest (the
 * reverse call) -> native (this). */
static ULONG thunk_call_iface( void )
{
    IMFAttributes *attrs = NULL;
    UINT32 count = 0xffffffff;
    ULONG ok = 0;

    if (SUCCEEDED(MFCreateAttributes( &attrs, 4 )) && attrs)
    {
        if (SUCCEEDED(IMFAttributes_SetUINT32( attrs, &IID_IUnknown, 0x5EED )) &&
            SUCCEEDED(IMFAttributes_GetCount( attrs, &count )) && count == 1)
            ok = 1;
        IMFAttributes_Release( attrs );
    }
    return ok;
}

struct guest_attrs
{
    IMFAttributes IMFAttributes_iface;
    LONG refs;
};

static struct guest_attrs the_attrs;

static inline struct guest_attrs *attrs_from( IMFAttributes *iface )
{
    return (struct guest_attrs *)iface;
}

static HRESULT WINAPI ga_QueryInterface( IMFAttributes *iface, REFIID riid, void **out_p )
{
    MARK( 0 );
    if (!out_p) return E_POINTER;
    *out_p = NULL;
    if (guid_eq( riid, &IID_IUnknown ) || guid_eq( riid, &IID_IMFAttributes ))
    {
        seen.qi_count++;
        *out_p = iface;
        IMFAttributes_AddRef( iface );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI ga_AddRef( IMFAttributes *iface )
{
    struct guest_attrs *o = attrs_from( iface );
    LONG r;

    MARK( 1 );
    r = InterlockedIncrement( &o->refs );
    if ((ULONG)r > seen.peak_refs) seen.peak_refs = (ULONG)r;
    return (ULONG)r;
}

static ULONG WINAPI ga_Release( IMFAttributes *iface )
{
    struct guest_attrs *o = attrs_from( iface );

    MARK( 2 );
    return (ULONG)InterlockedDecrement( &o->refs );
}

/* Everything below is one of two things: a method the hook drives, which
 * records what it was told and answers WINECOM_ST_HR_OK so the RETURN value is
 * checkable too; or a method nothing should ever reach, which marks itself so
 * the run can say it was reached.  SetItem is the interesting member of the
 * second class: the tables refuse it in both directions, so reaching it at all
 * is the failure. */
#define GA_UNUSED( n, name, args, params )                                    \
    static HRESULT WINAPI ga_##name args { MARK( n ); (void)iface; params;     \
                                           return E_NOTIMPL; }

GA_UNUSED( 3,  GetItem, ( IMFAttributes *iface, REFGUID key, PROPVARIANT *value ),
           ((void)key, (void)value) )
GA_UNUSED( 4,  GetItemType, ( IMFAttributes *iface, REFGUID key, MF_ATTRIBUTE_TYPE *type ),
           ((void)key, (void)type) )
GA_UNUSED( 5,  CompareItem, ( IMFAttributes *iface, REFGUID key, REFPROPVARIANT value, BOOL *result ),
           ((void)key, (void)value, (void)result) )
GA_UNUSED( 6,  Compare, ( IMFAttributes *iface, IMFAttributes *theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL *result ),
           ((void)theirs, (void)type, (void)result) )
GA_UNUSED( 7,  GetUINT32, ( IMFAttributes *iface, REFGUID key, UINT32 *value ),
           ((void)key, (void)value) )
GA_UNUSED( 8,  GetUINT64, ( IMFAttributes *iface, REFGUID key, UINT64 *value ),
           ((void)key, (void)value) )
GA_UNUSED( 9,  GetDouble, ( IMFAttributes *iface, REFGUID key, double *value ),
           ((void)key, (void)value) )
GA_UNUSED( 10, GetGUID, ( IMFAttributes *iface, REFGUID key, GUID *value ),
           ((void)key, (void)value) )
GA_UNUSED( 11, GetStringLength, ( IMFAttributes *iface, REFGUID key, UINT32 *length ),
           ((void)key, (void)length) )
GA_UNUSED( 12, GetString, ( IMFAttributes *iface, REFGUID key, WCHAR *value, UINT32 size, UINT32 *length ),
           ((void)key, (void)value, (void)size, (void)length) )
GA_UNUSED( 13, GetAllocatedString, ( IMFAttributes *iface, REFGUID key, WCHAR **value, UINT32 *length ),
           ((void)key, (void)value, (void)length) )
GA_UNUSED( 14, GetBlobSize, ( IMFAttributes *iface, REFGUID key, UINT32 *size ),
           ((void)key, (void)size) )
GA_UNUSED( 15, GetBlob, ( IMFAttributes *iface, REFGUID key, UINT8 *buf, UINT32 bufsize, UINT32 *blobsize ),
           ((void)key, (void)buf, (void)bufsize, (void)blobsize) )
GA_UNUSED( 16, GetAllocatedBlob, ( IMFAttributes *iface, REFGUID key, UINT8 **buf, UINT32 *size ),
           ((void)key, (void)buf, (void)size) )

static HRESULT WINAPI ga_GetUnknown( IMFAttributes *iface, REFGUID key, REFIID riid,
                                     void **ppv )
{
    MARK( 17 );
    (void)iface; (void)key;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!seen.unknown) return MF_E_ATTRIBUTENOTFOUND;
    if (!guid_eq( riid, &IID_IUnknown )) return E_NOINTERFACE;
    /* Hand back exactly the pointer SetUnknown gave us, with the reference the
     * caller is entitled to.  The mechanism must turn it back into the native
     * object it wraps -- that is the native half of the round trip. */
    IUnknown_AddRef( seen.unknown );
    *ppv = seen.unknown;
    return S_OK;
}

static HRESULT WINAPI ga_SetItem( IMFAttributes *iface, REFGUID key, REFPROPVARIANT value )
{
    /* MUST NEVER BE REACHED.  A PROPVARIANT's `vt` can name an interface
     * pointer that no IID in the signature types, so the tables refuse this
     * slot for what it is rather than for which direction it travels -- and
     * the reverse dispatcher honours that refusal. */
    MARK( 18 );
    (void)iface; (void)key; (void)value;
    return S_OK;
}

GA_UNUSED( 19, DeleteItem, ( IMFAttributes *iface, REFGUID key ), ((void)key) )

/* THE FOREIGN-THREAD LEG.  Called by the hook from a thread NATIVE code
 * created, which had never run guest code before the reverse call that lands
 * here -- so this runs on whatever per-thread emulator state was built
 * lazily, one native frame ago.  It makes the same two thunked calls the
 * guest's own thread makes, because "a callback arrives" and "a callback can
 * do its job" are different claims and an audio mixer thread needs the
 * second one. */
static HRESULT WINAPI ga_DeleteAllItems( IMFAttributes *iface )
{
    MARK( 20 );
    (void)iface;
    seen.foreign_ran = 1;
    seen.tid_foreign = GetCurrentThreadId();
    seen.foreign_thunk_int_ok = thunk_call_int();
    seen.foreign_thunk_iface_ok = thunk_call_iface();
    return E_NOTIMPL;
}

static HRESULT WINAPI ga_SetUINT32( IMFAttributes *iface, REFGUID key, UINT32 value )
{
    MARK( 21 );
    (void)iface; (void)key;
    seen.uint32 = value;
    /* ...and, from inside this reverse call, an ordinary thunked API call. */
    seen.tid_main = GetCurrentThreadId();
    seen.thunk_int_ok = thunk_call_int();
    return WINECOM_ST_HR_OK;
}

static HRESULT WINAPI ga_SetUINT64( IMFAttributes *iface, REFGUID key, UINT64 value )
{
    MARK( 22 );
    (void)iface; (void)key;
    seen.uint64 = value;
    seen.thunk_iface_ok = thunk_call_iface();
    return WINECOM_ST_HR_OK;
}

static HRESULT WINAPI ga_SetDouble( IMFAttributes *iface, REFGUID key, double value )
{
    MARK( 23 );
    (void)iface; (void)key;
    seen.dbl = value;
    return WINECOM_ST_HR_OK;
}

GA_UNUSED( 24, SetGUID, ( IMFAttributes *iface, REFGUID key, REFGUID value ),
           ((void)key, (void)value) )

static HRESULT WINAPI ga_SetString( IMFAttributes *iface, REFGUID key, const WCHAR *value )
{
    UINT32 i;

    MARK( 25 );
    (void)iface; (void)key;
    seen.str[0] = 0;
    if (!value) return WINECOM_ST_HR_OK;
    for (i = 0; i < 63 && value[i]; i++) seen.str[i] = value[i];
    seen.str[i] = 0;
    return WINECOM_ST_HR_OK;
}

GA_UNUSED( 26, SetBlob, ( IMFAttributes *iface, REFGUID key, const UINT8 *buf, UINT32 size ),
           ((void)key, (void)buf, (void)size) )

static HRESULT WINAPI ga_SetUnknown( IMFAttributes *iface, REFGUID key, IUnknown *unknown )
{
    MARK( 27 );
    (void)iface; (void)key;
    if (!unknown) return WINECOM_ST_HR_OK;
    /* Keep it, and ask it for IID_IUnknown right here: the answer must be the
     * SAME POINTER, which is the interning claim on this side of the boundary
     * and the thing that makes a guest's own identity comparison work. */
    IUnknown_AddRef( unknown );
    seen.unknown = unknown;
    seen.unknown_qi_hr = IUnknown_QueryInterface( unknown, &IID_IUnknown,
                                                  (void **)&seen.unknown_qi );
    if (SUCCEEDED(seen.unknown_qi_hr) && seen.unknown_qi)
        IUnknown_Release( seen.unknown_qi );
    return WINECOM_ST_HR_OK;
}

/* THE NESTING LEG.  Reverse-called by __wine_winecom_reverse_nest; calls it
 * straight back, which is one guest->native thunk trap, which makes one more
 * native->guest reverse call.  Each round trip is two crossings and every
 * crossing spends kernel stack in a place nothing bounds. */
static unsigned int nest_depth, nest_target, nest_max;

static HRESULT WINAPI ga_LockStore( IMFAttributes *iface )
{
    MARK( 28 );
    nest_depth++;
    if (nest_depth > nest_max)
    {
        nest_max = nest_depth;
        note( "note: nest depth " ); note_dec( nest_max ); note( "\n" );
    }
    if (nest_depth < nest_target) __wine_winecom_reverse_nest( iface, &nest_max );
    nest_depth--;
    return S_OK;
}

static HRESULT WINAPI ga_UnlockStore( IMFAttributes *iface )
{
    MARK( 29 );
    (void)iface;
    return E_NOTIMPL;
}

static HRESULT WINAPI ga_GetCount( IMFAttributes *iface, UINT32 *count )
{
    MARK( 30 );
    (void)iface;
    if (!count) return E_POINTER;
    /* An integer travelling OUT of a reverse call, written into storage the
     * NATIVE caller owns.  The value is the number of Set* calls that have
     * landed here, so it is a fact about the run rather than a constant. */
    *count = (UINT32)(seen.entered[21] + seen.entered[22] + seen.entered[23] +
                      seen.entered[25] + seen.entered[27]);
    return S_OK;
}

GA_UNUSED( 31, GetItemByIndex, ( IMFAttributes *iface, UINT32 index, GUID *key, PROPVARIANT *value ),
           ((void)index, (void)key, (void)value) )
GA_UNUSED( 32, CopyAllItems, ( IMFAttributes *iface, IMFAttributes *dest ), ((void)dest) )

static const IMFAttributesVtbl ga_vtbl =
{
    ga_QueryInterface, ga_AddRef, ga_Release,
    ga_GetItem, ga_GetItemType, ga_CompareItem, ga_Compare,
    ga_GetUINT32, ga_GetUINT64, ga_GetDouble, ga_GetGUID,
    ga_GetStringLength, ga_GetString, ga_GetAllocatedString,
    ga_GetBlobSize, ga_GetBlob, ga_GetAllocatedBlob, ga_GetUnknown,
    ga_SetItem, ga_DeleteItem, ga_DeleteAllItems,
    ga_SetUINT32, ga_SetUINT64, ga_SetDouble, ga_SetGUID, ga_SetString,
    ga_SetBlob, ga_SetUnknown, ga_LockStore, ga_UnlockStore,
    ga_GetCount, ga_GetItemByIndex, ga_CopyAllItems,
};

/* ---- the second object, for the one argument class the first has none of:
 * a by-value SINGLE-precision float, which MS-x64 puts in the low half of an
 * XMM register and ELFv2 puts in an FPR in double format. ---- */

struct guest_volume
{
    IMFSimpleAudioVolume IMFSimpleAudioVolume_iface;
    LONG refs;
};

static struct guest_volume the_volume;

static HRESULT WINAPI gv_QueryInterface( IMFSimpleAudioVolume *iface, REFIID riid, void **out_p )
{
    if (!out_p) return E_POINTER;
    *out_p = NULL;
    if (guid_eq( riid, &IID_IUnknown ) ||
        guid_eq( riid, &IID_IMFSimpleAudioVolume ))
    {
        *out_p = iface;
        IMFSimpleAudioVolume_AddRef( iface );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI gv_AddRef( IMFSimpleAudioVolume *iface )
{
    return (ULONG)InterlockedIncrement( &((struct guest_volume *)iface)->refs );
}

static ULONG WINAPI gv_Release( IMFSimpleAudioVolume *iface )
{
    return (ULONG)InterlockedDecrement( &((struct guest_volume *)iface)->refs );
}

static HRESULT WINAPI gv_SetMasterVolume( IMFSimpleAudioVolume *iface, float level )
{
    (void)iface;
    seen.flt = level;
    seen.entered[33]++;
    return WINECOM_ST_HR_OK;
}

static HRESULT WINAPI gv_GetMasterVolume( IMFSimpleAudioVolume *iface, float *level )
{
    (void)iface;
    if (level) *level = seen.flt;
    return S_OK;
}

static HRESULT WINAPI gv_SetMute( IMFSimpleAudioVolume *iface, const BOOL mute )
{
    (void)iface; (void)mute;
    return E_NOTIMPL;
}

static HRESULT WINAPI gv_GetMute( IMFSimpleAudioVolume *iface, BOOL *mute )
{
    (void)iface;
    if (mute) *mute = FALSE;
    return S_OK;
}

static const IMFSimpleAudioVolumeVtbl gv_vtbl =
{
    gv_QueryInterface, gv_AddRef, gv_Release,
    gv_SetMasterVolume, gv_GetMasterVolume, gv_SetMute, gv_GetMute,
};

/* --------------------------------------------------------------- the run */

/* Declared here rather than by including the header a second time: the guest
 * build has no import library but the .def the check script writes, and the
 * declaration in include/wine/winecom_selftest.h is already visible above. */

static BOOL wstr_is( const WCHAR *a, const WCHAR *b )
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void WINAPI reverse_probe_entry(void)
{
    struct winecom_selftest_report report;
    LONG refs_before, refs_after;
    HRESULT hr;
    ULONG i;

    out( "reverse_probe: start\n" );

    the_attrs.IMFAttributes_iface.lpVtbl = (IMFAttributesVtbl *)&ga_vtbl;
    the_attrs.refs = 1;
    the_volume.IMFSimpleAudioVolume_iface.lpVtbl =
        (IMFSimpleAudioVolumeVtbl *)&gv_vtbl;
    the_volume.refs = 1;
    for (i = 0; i < 40; i++) seen.entered[i] = 0;
    seen.uint32 = 0; seen.uint64 = 0; seen.dbl = 0; seen.flt = 0;
    seen.str[0] = 0; seen.unknown = NULL; seen.unknown_qi = NULL;
    seen.unknown_qi_hr = E_FAIL; seen.qi_count = 0; seen.peak_refs = 1;

    hr = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    step( "MFStartup", hr == S_OK, NULL );
    if (hr != S_OK) goto done;

    refs_before = the_attrs.refs;

    /* ONE call across the boundary; everything after this is what came back. */
    hr = __wine_winecom_reverse_selftest( &the_attrs.IMFAttributes_iface,
                                          &the_volume.IMFSimpleAudioVolume_iface,
                                          &report );
    step( "the native hook drove the guest objects", hr == S_OK,
          hr == S_OK ? NULL : "the hook reported a failed check" );

    /* ---- what the GUEST received ---- */
    step( "SetUINT32 value", seen.entered[21] == 1 && seen.uint32 == WINECOM_ST_UINT32,
          NULL );
    step( "SetUINT64 value", seen.entered[22] == 1 && seen.uint64 == WINECOM_ST_UINT64,
          NULL );
    {
        double want_d = (double)WINECOM_ST_DOUBLE;
        float  want_f = (float)WINECOM_ST_FLOAT;
        ULONGLONG got_db, want_db;
        UINT32 got_fb, want_fb;

        /* Compared and printed as BIT PATTERNS.  A floating-point value that
         * lost its low bits crossing between an FPR and an XMM register is
         * still "about right" as a decimal, and about right is the failure
         * this whole layer exists to turn into a loud one. */
        __builtin_memcpy( &got_db,  &seen.dbl, 8 );
        __builtin_memcpy( &want_db, &want_d,   8 );
        __builtin_memcpy( &got_fb,  &seen.flt, 4 );
        __builtin_memcpy( &want_fb, &want_f,   4 );
        out( "fp: double got=" ); out_hex( got_db, 16 );
        out( " want=" ); out_hex( want_db, 16 );
        out( "  float got=" ); out_hex( got_fb, 8 );
        out( " want=" ); out_hex( want_fb, 8 );
        out( "\n" );
        step( "SetDouble value (a by-value double the forward direction refuses)",
              seen.entered[23] == 1 && got_db == want_db, NULL );
        step( "SetMasterVolume value (a by-value single-precision float)",
              seen.entered[33] == 1 && got_fb == want_fb, NULL );
    }
    step( "SetString value", seen.entered[25] == 1 &&
          wstr_is( seen.str, WINECOM_ST_STRING ), NULL );
    step( "SetUnknown gave the guest a usable interface pointer",
          seen.entered[27] == 1 && seen.unknown != NULL, NULL );
    step( "that interface pointer is one of the guest's own address space",
          seen.unknown != NULL && (ULONG_PTR)seen.unknown != 0, NULL );
    step( "QueryInterface(IID_IUnknown) on it came back as the SAME pointer "
          "(guest-side interning)",
          SUCCEEDED(seen.unknown_qi_hr) && seen.unknown_qi == seen.unknown, NULL );
    step( "GetUnknown was entered", seen.entered[17] == 1, NULL );

    /* ---- the pattern DOOM died on ---- */
    step( "a reverse-called guest method made an ordinary THUNKED call back "
          "into native code (ints)", seen.thunk_int_ok == 1,
          "the nested run's trap dispatch did not consume the guest thunk "
          "stub's trap" );
    step( "...and one that VENDS AN INTERFACE, then called a method on it",
          seen.thunk_iface_ok == 1,
          "a flat wrapper and a COM vtable slot, three crossings deep" );
    step( "GetCount was entered and answered", seen.entered[30] == 1, NULL );

    /* ---- the refusal discipline ---- */
    step( "SetItem (a PROPVARIANT slot) was NEVER entered", seen.entered[18] == 0,
          seen.entered[18] ? "the reverse dispatcher served a refused row" : NULL );

    /* ---- what the NATIVE side found ---- */
    out( "hook: checks=" ); out_dec( report.checks );
    out( " failures=" ); out_dec( report.failures );
    out( " first_fail=" ); out_dec( report.first_fail );
    out( " calls=" ); out_dec( report.calls );
    out( " guest_count=" ); out_dec( report.guest_count );
    out( "\n" );
    /* Twelve: one per argument class the hook drives, plus the round trip,
     * the refusal, the identity and the reference balance.  Asserted as an
     * exact count rather than a floor, so a check that quietly stopped running
     * is a failure here and not an absence nobody notices. */
    step( "the native side ran every check", report.checks == 12, NULL );
    step( "the native side failed none", report.failures == 0, NULL );
    step( "native round trip: the object the guest was handed came back as "
          "itself", report.roundtrip_ok == 1, NULL );
    step( "native identity: QueryInterface on the reverse proxy came back as "
          "the same proxy", report.identity_ok == 1, NULL );
    step( "native refusal: SetItem answered E_NOTIMPL", report.refuse_ok == 1, NULL );
    step( "native reference balance across AddRef/Release",
          report.refs_leaked == 0, NULL );
    step( "GetCount crossed OUT with the value the guest wrote",
          report.guest_count == 5, NULL );

    /* ---- what one crossing costs.  A `note:`-shaped line rather than a
     * step, because a duration is not a pass/fail -- but the FOREIGN-THREAD
     * leg is, because a reverse call that never arrives on a thread native
     * code created is the failure mode every work queue and every audio mixer
     * would hit. ---- */
    note( "note: one call costs native=" ); note_dec( report.ns_native );
    note( "ns reverse=" ); note_dec( report.ns_reverse );
    note( "ns first-on-a-foreign-thread=" ); note_dec( report.ns_foreign );
    note( "ns, over " ); note_dec( WINECOM_ST_TIMED_CALLS );
    note( " repetitions of IMFAttributes::DeleteItem\n" );
    step( "a reverse call arrives on a thread NATIVE code created, which had "
          "never run guest code", report.foreign_ok == 1 && seen.foreign_ran == 1,
          "the emulator's lazy per-thread init is what makes an MF work queue "
          "and an XAudio2 mixer thread work at all" );
    step( "that foreign thread is a DIFFERENT thread from the one that called in",
          seen.tid_foreign != 0 && seen.tid_foreign != seen.tid_main, NULL );
    step( "and the reverse-called method could make a THUNKED call from it "
          "(ints)", seen.foreign_thunk_int_ok == 1,
          "this is the XAudio2 mixer thread's exact shape: OnBufferEnd -> "
          "SubmitSourceBuffer" );
    step( "...and one that VENDS AN INTERFACE, from that same foreign thread",
          seen.foreign_thunk_iface_ok == 1, NULL );
    step( "the guest saw every timed call (the timing loop is measuring "
          "crossings, not a native no-op)",
          seen.entered[19] >= WINECOM_ST_TIMED_CALLS, NULL );

    /* ---- HOW DEEP THE PING-PONG GOES.  A callback-driven API nests
     * guest->native->guest without a bound the port controls, and every
     * crossing spends kernel stack.  Run LAST, because if the depth is not
     * survivable this is where the run ends. ---- */
    nest_depth = 0;
    nest_max = 0;
    nest_target = WINECOM_ST_NEST_DEPTH;
    hr = __wine_winecom_reverse_nest( &the_attrs.IMFAttributes_iface, &nest_max );
    out( "nest: reached " ); out_dec( nest_max );
    out( " of " ); out_dec( nest_target );
    out( "\n" );
    step( "guest<->native ping-pong survives the depth a callback-driven API "
          "reaches", hr == S_OK && nest_max == nest_target,
          "each round trip is a guest->native trap and a native->guest reverse "
          "call, and both spend kernel stack" );

    /* ---- the guest object's own reference count ---- */
    refs_after = the_attrs.refs;
    out( "refs: before=" ); out_dec( (ULONG)refs_before );
    out( " peak=" ); out_dec( seen.peak_refs );
    out( " after=" ); out_dec( (ULONG)refs_after );
    out( " qi_entered=" ); out_dec( seen.qi_count );
    out( "\n" );
    step( "the guest object's reference count is back where it started",
          refs_after == refs_before,
          refs_after == refs_before ? NULL
                                    : "native code kept a reference it should "
                                      "have dropped, or dropped one twice" );
    step( "the reverse proxy DID take a reference while it lived",
          seen.peak_refs > (ULONG)refs_before,
          "a proxy that never AddRef'd the guest object would let it die under "
          "native code" );
    step( "QueryInterface reached the guest object", seen.qi_count >= 1, NULL );

    MFShutdown();

done:
    out( failures ? "reverse_probe: FAIL " : "reverse_probe: PASS " );
    out_dec( (ULONG)(checks - failures) );
    out( "/" );
    out_dec( (ULONG)checks );
    out( "\n" );
    ExitProcess( failures ? 1 : 0 );
}
