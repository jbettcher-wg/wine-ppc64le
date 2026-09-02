/*
 * hand_hygiene_probe.c -- the guest probe behind
 * ppc64le/winecom/check-hand-hygiene.sh.
 *
 * WHAT IT IS FOR.  libs/winecom's scrub_refused_outs() serves TABLE refusals
 * only, and the table-scrub gate (check-com-levers.sh, leg D) can therefore
 * see nothing that happens in a WINECOM_F_HAND walker or a flat GUEST-IMPL
 * wrapper.  That blind spot is exactly how six unscrubbed hand refusals
 * shipped in dlls/combase/syscom.c, and one of them -- IMMDevice::Activate
 * with a non-NULL pActivationParams -- cost days: the Witcher 3 read the
 * never-written *ppv back off its own stack and the emulator decoded a host
 * module's ppc64le bytes as x86 (690567d9d8e).  This probe is that blind spot
 * closed, FROM THE CALLER'S CHAIR:
 *
 *   activate_hr=/activate_out=
 *              IMMDevice::Activate( IID_IAudioClient, CLSCTX_ALL, &params,
 *              &ppv ) with a non-NULL PROPVARIANT -- the canonical hand-walker
 *              refusal, and the live one.  The out cell is seeded with the
 *              residue-shaped sentinel the Witcher 3 actually called through:
 *                null      the walker refused and SCRUBBED -- refused is INERT
 *                sentinel  the walker refused and did NOT scrub, which is only
 *                          correct under WINEEMUNOREFUSESCRUB=1 and is the
 *                          crash class everywhere else
 *                object    it SERVED, which means the refusal this arm exists
 *                          to measure did not happen and the arm is void
 *
 *   dinput_hr=/dinput_out=
 *              DirectInput8Create( ..., &out, punkOuter ) with a non-NULL
 *              aggregation outer -- the same three-way split, in a DIFFERENT
 *              FILE (dlls/dinput8/guestcom.c) and on the FLAT wrapper path
 *              rather than the walker path.  One file's scrub proving
 *              load-bearing does not prove the helper reaches the others.
 *
 * Both refusals are UNCONDITIONAL given their argument, which is what makes
 * them gate material: there is no environment in which they serve, so the
 * only thing that can change between the two arms is the scrub itself.
 *
 * The probe asserts NOTHING and always exits 0 after printing.  What is
 * correct depends on which levers the runner set, and the runner is
 * check-hand-hygiene.sh; a probe that decided for itself would have to know
 * the environment, and then the gate would be testing the probe.
 *
 * GUEST-ONLY, and no CRT -- the same rule as com_lever_smoke.c beside it and
 * for the same reason: the program formats its own output and writes it with
 * WriteFile, so nothing but the COM boundary is under test.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

/* Spelled out rather than linked from libuuid: the guest build has no Wine
 * import libraries at all.  Verified against include/mmdeviceapi.idl
 * (the MMDeviceEnumerator coclass and IMMDeviceEnumerator) and
 * include/audioclient.idl (IAudioClient). */
static const GUID hyg_CLSID_MMDeviceEnumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e } };
static const GUID hyg_IID_IMMDeviceEnumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6 } };
static const GUID hyg_IID_IAudioClient =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2 } };
/* IID_IDirectInput8W, from include/dinput.h */
static const GUID hyg_IID_IDirectInput8W =
    { 0xbf798031, 0x483a, 0x4da2, { 0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00 } };

/* the residue-shaped sentinel from the Witcher 3 lesson, verbatim from
 * com_lever_smoke.c so the two probes name the same value */
#define HYG_SENTINEL ((void *)(UINT_PTR)0x5AB07A6E5AB07A6EULL)

HRESULT WINAPI DirectInput8Create( HINSTANCE, DWORD, REFIID, void **, IUnknown * );

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex8( ULONG v )
{
    static const char digits[] = "0123456789abcdef";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) { buf[i] = digits[v & 0xf]; v >>= 4; }
    buf[8] = 0;
    out( buf );
}

/* The whole observable: what the guest finds in its own out cell after a
 * refusal it seeded with the sentinel first. */
static void cell_verdict( const void *got )
{
    out( got == NULL ? "null" :
         got == HYG_SENTINEL ? "sentinel" : "object" );
}

/* ---- arm 1: the hand walker, dlls/combase/syscom.c ----------------------- */
static void activate_leg( void )
{
    IMMDeviceEnumerator *devenum = NULL;
    IMMDevice *device = NULL;
    void *ppv = HYG_SENTINEL;
    PROPVARIANT params;
    HRESULT hr;
    UINT i;

    /* A zeroed PROPVARIANT is VT_EMPTY, which is a perfectly valid one.  What
     * the walker refuses is its ADDRESS being non-NULL at all -- the union has
     * an IUnknown arm and nothing here can type it -- so this is the refusal
     * with the least possible ceremony around it. */
    for (i = 0; i < sizeof(params); i++) ((BYTE *)&params)[i] = 0;

    hr = CoCreateInstance( &hyg_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                           &hyg_IID_IMMDeviceEnumerator, (void **)&devenum );
    if (FAILED(hr) || !devenum)
    {
        out( "activate_hr=0x" ); out_hex8( (ULONG)hr );
        out( " activate_out=noenum\n" );
        return;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint( devenum, eRender, eConsole,
                                                      &device );
    if (FAILED(hr) || !device)
    {
        out( "activate_hr=0x" ); out_hex8( (ULONG)hr );
        out( " activate_out=nodevice\n" );
        IMMDeviceEnumerator_Release( devenum );
        return;
    }

    hr = IMMDevice_Activate( device, &hyg_IID_IAudioClient, CLSCTX_ALL,
                             &params, &ppv );
    out( "activate_hr=0x" );
    out_hex8( (ULONG)hr );
    out( " activate_out=" );
    cell_verdict( ppv );
    out( "\n" );

    if (SUCCEEDED(hr) && ppv && ppv != HYG_SENTINEL)
        IUnknown_Release( (IUnknown *)ppv );
    IMMDevice_Release( device );
    IMMDeviceEnumerator_Release( devenum );
}

/* ---- arm 2: the flat wrapper, dlls/dinput8/guestcom.c -------------------- */
static void dinput_leg( void )
{
    /* An IUnknown-shaped address that is not NULL is all the refusal needs;
     * it is never dereferenced, because refusing is the whole answer. */
    IUnknown *outer = (IUnknown *)(UINT_PTR)0x1000;
    void *obj = HYG_SENTINEL;
    HRESULT hr;

    hr = DirectInput8Create( GetModuleHandleW( NULL ), 0x0800,
                             &hyg_IID_IDirectInput8W, &obj, outer );
    out( "dinput_hr=0x" );
    out_hex8( (ULONG)hr );
    out( " dinput_out=" );
    cell_verdict( obj );
    out( "\n" );
    if (SUCCEEDED(hr) && obj && obj != HYG_SENTINEL)
        IUnknown_Release( (IUnknown *)obj );
}

void __stdcall hand_hygiene_entry( void )
{
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    activate_leg();
    dinput_leg();
    CoUninitialize();
    out( "hand_hygiene_probe: done\n" );
    ExitProcess( 0 );
}
