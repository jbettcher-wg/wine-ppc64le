/*
 * ds_smoke -- the native-vs-guest DirectSound runtime gate.
 *
 * ONE source, built TWICE and run TWICE under the same wine: once as a NATIVE
 * ppc64 PE (winegcc, this machine's own architecture, no emulation anywhere in
 * the process) and once as an x86-64 Windows PE run as a GUEST.  The two runs
 * must print BYTE-IDENTICAL stdout.
 *
 * Both legs reach the SAME implementation -- Wine's own dlls/dsound, its own
 * mixer, its own mmdevapi and the same host audio backend.  The only thing
 * that differs is whether the caller's instructions are ppc64 or x86-64, and
 * therefore whether every one of these calls crossed the guest COM boundary.
 * Identical bytes out means the boundary changed nothing: not an argument, not
 * a returned interface pointer, not a float, not a buffer of PCM.
 *
 * WHAT EACH STEP IS FOR
 *
 *   1-2  DirectSoundCreate8 and SetCooperativeLevel.  The flat export is
 *        GUEST-IMPL-redirected to a wrapper that wraps the IDirectSound8 it
 *        returned; without that the guest would hold a NATIVE vtable and its
 *        first method call would execute ppc64 bytes as x86-64.
 *   3    GetCaps: a struct the implementation FILLS IN, read back through the
 *        boundary and value-checked against the format asked for below.
 *   4    a secondary buffer -- the hand-written CreateSoundBuffer slot, whose
 *        third argument is an aggregation pUnkOuter.
 *   5    QueryInterface for IDirectSoundBuffer8.  This is served entirely by
 *        libs/winecom from the proxy table, and it is the step that proves the
 *        roster's IIDs are the ones the implementation answers to.
 *   6    GetFormat: the format the implementation believes it has.
 *   7-8  Lock, WRITE A KNOWN PCM PATTERN, Unlock, Lock again and read it back.
 *        Lock's `LPVOID *` out-parameters are the one place on this surface
 *        where a void** is NOT an interface pointer, and the checksum is the
 *        proof that the pointer the guest got addresses the very bytes the
 *        mixer will play rather than a copy.
 *   9-10 the DWORD/LONG state machine: status before play, and volume, pan and
 *        frequency set and read back exactly.
 *  11-12 Play, then the play cursor ADVANCING.  A cursor that moves is the
 *        proof that the mixer thread is running against a real device and that
 *        this is not a paper object; the raw cursor value is not printed,
 *        because it is a function of when the poll ran.
 *  13    IDirectSoundNotify with a real event HANDLE inside a struct the
 *        implementation reads.  Both sides are the same Wine object namespace,
 *        so the handle needs no translation -- and this step is what says so.
 *  14-15 Stop, status again, Restore.
 *  16-19 THE FLOATING-POINT BOUNDARY, on IDirectSound3DListener.  MS-x64 puts
 *        the first four arguments in XMM0..XMM3 when they are floating point
 *        and on the STACK after that, while ELFv2 passes floats in f1..f13; a
 *        value that crossed through an integer register would be a WRONG
 *        NUMBER, not a crash.  SetOrientation is the one that matters most:
 *        it takes SIX floats, so its fourth, fifth and sixth travel on the
 *        guest's stack.  Every constant here is a dyadic fraction, so the
 *        comparison is on the RAW BITS and the last bit counts.
 *  20    the whole thing comes apart: every proxy released, refcount to zero.
 *
 * OBSERVATIONS THAT ARE TRUE OF THE PORT AND NOT OF DIRECTSOUND go to stderr
 * as `note:` lines and never enter the diffed transcript, because their answer
 * legitimately differs between the legs.  There is one: passing a non-NULL
 * aggregation pUnkOuter.  Wine's own dsound IGNORES that argument, so the
 * native leg gets DS_OK; the guest leg is REFUSED with DSERR_NOAGGREGATION,
 * because aggregation hands native code a guest-implemented IUnknown and this
 * port has no reverse proxies.  check-audio-smoke.sh requires the guest note
 * to say exactly that.
 *
 * DS_SMOKE_BREAK (falsification; the gate builds each variant and requires it
 * to FAIL, because a gate that cannot go red proves nothing):
 *
 *   =1  do not write the pattern at all, so the read-back checksum in step 8
 *       is the checksum of whatever the buffer already held.
 *   =2  check one byte of the read-back instead of all of it -- coverage is
 *       part of step 8's claim, so its verdict fails on the arithmetic.
 *   =3  corrupt the expected orientation bits, so step 19 goes red: proof
 *       that the six-float check is a check and not a formality.
 *
 * NO C RUNTIME on the guest leg, for the reason gl_smoke.c gives: the program
 * formats its own output and writes it with WriteFile, so neither libc's nor
 * ucrt's printf can be the source of a byte difference.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef DS_SMOKE_BREAK
#define DS_SMOKE_BREAK 0
#endif

#include <initguid.h>
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

/* ------------------------------------------------------------- output */

static void out_fd( HANDLE h, const char *s )
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile( h, s, n, &written, NULL );
}

static void out( const char *s )
{
    out_fd( GetStdHandle( STD_OUTPUT_HANDLE ), s );
}

/* Assembled and written in ONE call: stderr is where the port's own debug
 * channels write too, and a line emitted in three WriteFiles can arrive with
 * an err:winecom line spliced through the middle of it. */
static void note( const char *label, LONG v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[160];
    int n = 0, i;

    while (label[n] && n < 120) { buf[n] = label[n]; n++; }
    buf[n++] = ' '; buf[n++] = '0'; buf[n++] = 'x';
    for (i = 7; i >= 0; i--) buf[n++] = hex[((ULONG)v >> (4 * i)) & 0xf];
    buf[n++] = '\n';
    buf[n] = 0;
    out_fd( GetStdHandle( STD_ERROR_HANDLE ), buf );
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
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

static void out_sdec( LONG v )
{
    if (v < 0) { out( "-" ); out_dec( (ULONG)(-v) ); }
    else out_dec( (ULONG)v );
}

/* The bits of a float, so a floating-point value can be compared and printed
 * EXACTLY.  A decimal rendering would need a formatter this probe deliberately
 * does not have, and would hide the last bit -- which is the bit a marshalling
 * bug moves. */
static ULONG fbits( float f )
{
    union { float f; ULONG u; } c;
    c.f = f;
    return c.u;
}

/* 32-bit FNV-1a. */
static DWORD fnv1a( DWORD hash, const BYTE *p, UINT n )
{
    UINT i;
    for (i = 0; i < n; i++)
    {
        hash ^= p[i];
        hash *= 0x01000193u;
    }
    return hash;
}

/* No CRT on the guest leg, so no memset. */
static void zero( void *p, UINT n )
{
    BYTE *b = p;
    UINT i;
    for (i = 0; i < n; i++) b[i] = 0;
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;
static const char *first_fail;

static void begin( const char *what )
{
    out( "step " );
    out_dec( (ULONG)++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        if (!first_fail) first_fail = why;
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

/* The PCM pattern.  Deterministic, one byte at a time, and NOT all one value:
 * a constant fill would still checksum equal if Lock handed back a different
 * region of a zeroed buffer. */
static BYTE pattern_byte( UINT i )
{
    return (BYTE)(((i * 2654435761u) >> 13) ^ (i * 131u));
}

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BITS          16
#define BLOCK_ALIGN   (CHANNELS * BITS / 8)
#define BUFFER_BYTES  (SAMPLE_RATE * BLOCK_ALIGN)     /* exactly one second */

static int ds_smoke_run( void )
{
    IDirectSound8 *ds = NULL;
    IDirectSoundBuffer *buf = NULL, *primary = NULL;
    IDirectSoundBuffer8 *buf8 = NULL;
    IDirectSoundNotify *notify = NULL;
    IDirectSound3DListener *listener = NULL;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;
    DSCAPS caps;
    HRESULT hr;
    HANDLE ev = NULL;

    /* ---- 1: the device --------------------------------------------- */
    begin( "DirectSoundCreate8" );
    hr = DirectSoundCreate8( NULL, &ds, NULL );
    out_hr( "hr", hr );
    out( " obj=" ); out( ds ? "yes" : "no" );
    verdict( hr == DS_OK && ds != NULL, "no IDirectSound8" );
    if (!ds) goto done;

    begin( "SetCooperativeLevel" );
    hr = IDirectSound8_SetCooperativeLevel( ds, GetDesktopWindow(), DSSCL_PRIORITY );
    out_hr( "hr", hr );
    verdict( hr == DS_OK, "cooperative level refused" );

    /* ---- 3: GetCaps ------------------------------------------------- */
    begin( "GetCaps" );
    zero( &caps, sizeof(caps) );
    caps.dwSize = sizeof(caps);
    hr = IDirectSound8_GetCaps( ds, &caps );
    out_hr( "hr", hr );
    out( " size=" ); out_dec( caps.dwSize );
    out( " rate_ok=" );
    out( (caps.dwMinSecondarySampleRate <= SAMPLE_RATE &&
          caps.dwMaxSecondarySampleRate >= SAMPLE_RATE) ? "yes" : "no" );
    out( " flags_set=" ); out( caps.dwFlags ? "yes" : "no" );
    verdict( hr == DS_OK && caps.dwSize == sizeof(caps) &&
             caps.dwMinSecondarySampleRate <= SAMPLE_RATE &&
             caps.dwMaxSecondarySampleRate >= SAMPLE_RATE && caps.dwFlags,
             "GetCaps did not describe a device that can play 44100" );

    /* ---- 4: a secondary buffer -------------------------------------- */
    zero( &wfx, sizeof(wfx) );
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = BITS;
    wfx.nBlockAlign     = BLOCK_ALIGN;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * BLOCK_ALIGN;

    zero( &desc, sizeof(desc) );
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN |
                         DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2 |
                         DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLPOSITIONNOTIFY;
    desc.dwBufferBytes = BUFFER_BYTES;
    desc.lpwfxFormat   = &wfx;

    begin( "CreateSoundBuffer secondary" );
    hr = IDirectSound8_CreateSoundBuffer( ds, &desc, &buf, NULL );
    out_hr( "hr", hr );
    out( " obj=" ); out( buf ? "yes" : "no" );
    verdict( hr == DS_OK && buf != NULL, "no secondary buffer" );
    if (!buf) goto done;

    /* ---- 5: QueryInterface ------------------------------------------ */
    begin( "QueryInterface IDirectSoundBuffer8" );
    hr = IDirectSoundBuffer_QueryInterface( buf, &IID_IDirectSoundBuffer8,
                                            (void **)&buf8 );
    out_hr( "hr", hr );
    out( " obj=" ); out( buf8 ? "yes" : "no" );
    verdict( hr == DS_OK && buf8 != NULL, "no IDirectSoundBuffer8" );
    if (!buf8) goto done;

    /* ---- 6: GetFormat ----------------------------------------------- */
    {
        WAVEFORMATEX got;
        DWORD written = 0;

        begin( "GetFormat" );
        zero( &got, sizeof(got) );
        hr = IDirectSoundBuffer8_GetFormat( buf8, &got, sizeof(got), &written );
        out_hr( "hr", hr );
        out( " ch=" ); out_dec( got.nChannels );
        out( " rate=" ); out_dec( got.nSamplesPerSec );
        out( " bits=" ); out_dec( got.wBitsPerSample );
        out( " align=" ); out_dec( got.nBlockAlign );
        verdict( hr == DS_OK && got.nChannels == CHANNELS &&
                 got.nSamplesPerSec == SAMPLE_RATE &&
                 got.wBitsPerSample == BITS && got.nBlockAlign == BLOCK_ALIGN,
                 "the buffer does not have the format it was asked for" );
    }

    /* ---- 7-8: Lock, write, read back -------------------------------- */
    {
        void *p1 = NULL, *p2 = NULL;
        DWORD b1 = 0, b2 = 0;
        DWORD wrote = 0x811c9dc5u, read_back = 0x811c9dc5u;
        UINT i, checked = 0;

        begin( "Lock whole buffer" );
        hr = IDirectSoundBuffer8_Lock( buf8, 0, BUFFER_BYTES, &p1, &b1, &p2, &b2, 0 );
        out_hr( "hr", hr );
        out( " ptr1=" ); out( p1 ? "yes" : "no" );
        out( " bytes1=" ); out_dec( b1 );
        out( " ptr2=" ); out( p2 ? "yes" : "no" );
        out( " bytes2=" ); out_dec( b2 );
        verdict( hr == DS_OK && p1 && b1 == BUFFER_BYTES && b2 == 0,
                 "Lock did not hand back the whole buffer in one piece" );

        if (hr == DS_OK && p1)
        {
#if DS_SMOKE_BREAK != 1
            for (i = 0; i < b1; i++) ((BYTE *)p1)[i] = pattern_byte( i );
#endif
            for (i = 0; i < b1; i++)
            {
                BYTE b = pattern_byte( i );
                wrote = fnv1a( wrote, &b, 1 );
            }
            IDirectSoundBuffer8_Unlock( buf8, p1, b1, p2, b2 );

            p1 = p2 = NULL; b1 = b2 = 0;
            begin( "read the pattern back" );
            hr = IDirectSoundBuffer8_Lock( buf8, 0, BUFFER_BYTES, &p1, &b1,
                                            &p2, &b2, 0 );
            if (hr == DS_OK && p1)
            {
#if DS_SMOKE_BREAK == 2
                read_back = fnv1a( read_back, (const BYTE *)p1, 1 );
                checked = 1;
#else
                read_back = fnv1a( read_back, (const BYTE *)p1, b1 );
                checked = b1;
#endif
                IDirectSoundBuffer8_Unlock( buf8, p1, b1, p2, b2 );
            }
            out( "checked=" ); out_dec( checked );
            out( " wrote=0x" ); out_hex( wrote, 8 );
            out( " read=0x" ); out_hex( read_back, 8 );
            verdict( checked == BUFFER_BYTES && read_back == wrote,
                     "the bytes written through the boundary are not the "
                     "bytes the implementation holds" );
        }
    }

    /* ---- 9: status before play -------------------------------------- */
    {
        DWORD status = 0xdeadbeef;

        begin( "GetStatus before Play" );
        hr = IDirectSoundBuffer8_GetStatus( buf8, &status );
        out_hr( "hr", hr );
        out( " status=0x" ); out_hex( status, 8 );
        verdict( hr == DS_OK && !(status & DSBSTATUS_PLAYING),
                 "a fresh buffer says it is playing" );
    }

    /* ---- 10: the LONG/DWORD state machine --------------------------- */
    {
        LONG vol = 0, pan = 0;
        DWORD freq = 0;

        begin( "volume, pan and frequency round-trip" );
        hr = IDirectSoundBuffer8_SetVolume( buf8, -1234 );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_GetVolume( buf8, &vol );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_SetPan( buf8, 567 );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_GetPan( buf8, &pan );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_SetFrequency( buf8, 22050 );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_GetFrequency( buf8, &freq );
        out_hr( "hr", hr );
        out( " vol=" ); out_sdec( vol );
        out( " pan=" ); out_sdec( pan );
        out( " freq=" ); out_dec( freq );
        verdict( hr == DS_OK && vol == -1234 && pan == 567 && freq == 22050,
                 "a value did not survive the round trip" );

        IDirectSoundBuffer8_SetFrequency( buf8, DSBFREQUENCY_ORIGINAL );
        IDirectSoundBuffer8_SetVolume( buf8, DSBVOLUME_MIN );
    }

    /* ---- 11-12: Play, and the cursor moving -------------------------- */
    {
        DWORD status = 0, play0 = 0, write0 = 0, play1 = 0, write1 = 0;
        DWORD start;
        BOOL advanced = FALSE;

        begin( "Play" );
        IDirectSoundBuffer8_SetCurrentPosition( buf8, 0 );
        hr = IDirectSoundBuffer8_Play( buf8, 0, 0, 0 );
        if (hr == DS_OK) hr = IDirectSoundBuffer8_GetStatus( buf8, &status );
        out_hr( "hr", hr );
        out( " status=0x" ); out_hex( status, 8 );
        verdict( hr == DS_OK && (status & DSBSTATUS_PLAYING),
                 "the buffer did not start playing" );

        begin( "the play cursor advances" );
        IDirectSoundBuffer8_GetCurrentPosition( buf8, &play0, &write0 );
        start = GetTickCount();
        while (GetTickCount() - start < 2000)
        {
            Sleep( 20 );
            if (IDirectSoundBuffer8_GetCurrentPosition( buf8, &play1, &write1 ) != DS_OK)
                break;
            if (play1 != play0) { advanced = TRUE; break; }
        }
        /* The cursor VALUES are a function of when the poll ran and are
         * deliberately not printed; that it moved at all is the claim. */
        out( "advanced=" ); out( advanced ? "yes" : "no" );
        verdict( advanced, "the play cursor never moved -- the mixer is not "
                 "running against a device" );
    }

    /* ---- 13: IDirectSoundNotify, with a real event handle ------------ */
    {
        DSBPOSITIONNOTIFY pos;
        DWORD waited;

        begin( "IDirectSoundNotify with an event handle" );
        IDirectSoundBuffer8_Stop( buf8 );
        hr = IDirectSoundBuffer8_QueryInterface( buf8, &IID_IDirectSoundNotify,
                                                  (void **)&notify );
        if (hr == DS_OK)
        {
            ev = CreateEventW( NULL, FALSE, FALSE, NULL );
            zero( &pos, sizeof(pos) );
            pos.dwOffset = (DWORD)DSBPN_OFFSETSTOP;
            pos.hEventNotify = ev;
            hr = IDirectSoundNotify_SetNotificationPositions( notify, 1, &pos );
        }
        if (hr == DS_OK)
        {
            IDirectSoundBuffer8_SetCurrentPosition( buf8, 0 );
            hr = IDirectSoundBuffer8_Play( buf8, 0, 0, 0 );
        }
        if (hr == DS_OK) hr = IDirectSoundBuffer8_Stop( buf8 );
        waited = (hr == DS_OK) ? WaitForSingleObject( ev, 3000 ) : WAIT_FAILED;
        out_hr( "hr", hr );
        out( " signalled=" ); out( waited == WAIT_OBJECT_0 ? "yes" : "no" );
        verdict( hr == DS_OK && waited == WAIT_OBJECT_0,
                 "the stop notification never arrived" );
    }

    /* ---- 14-15: stopped, and restorable ------------------------------ */
    {
        DWORD status = 0xdeadbeef;

        begin( "GetStatus after Stop" );
        hr = IDirectSoundBuffer8_GetStatus( buf8, &status );
        out_hr( "hr", hr );
        out( " status=0x" ); out_hex( status, 8 );
        verdict( hr == DS_OK && !(status & DSBSTATUS_PLAYING),
                 "the buffer is still playing after Stop" );

        begin( "Restore" );
        hr = IDirectSoundBuffer8_Restore( buf8 );
        out_hr( "hr", hr );
        verdict( hr == DS_OK, "Restore refused" );
    }

    /* ---- 16-19: the floating-point boundary -------------------------- */
    {
        DSBUFFERDESC pdesc;

        begin( "primary buffer with 3D control" );
        zero( &pdesc, sizeof(pdesc) );
        pdesc.dwSize  = sizeof(pdesc);
        pdesc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
        hr = IDirectSound8_CreateSoundBuffer( ds, &pdesc, &primary, NULL );
        out_hr( "hr", hr );
        out( " obj=" ); out( primary ? "yes" : "no" );
        verdict( hr == DS_OK && primary != NULL, "no 3D primary buffer" );

        if (primary)
        {
            begin( "QueryInterface IDirectSound3DListener" );
            hr = IDirectSoundBuffer_QueryInterface( primary,
                     &IID_IDirectSound3DListener, (void **)&listener );
            out_hr( "hr", hr );
            out( " obj=" ); out( listener ? "yes" : "no" );
            verdict( hr == DS_OK && listener != NULL, "no 3D listener" );
        }
    }

    if (listener)
    {
        D3DVALUE f = 0.0f;
        D3DVECTOR v, front, top;

        /* One float in XMM1, one DWORD past it. */
        begin( "3D distance factor round-trip (1 float)" );
        hr = IDirectSound3DListener_SetDistanceFactor( listener, 2.5f,
                                                        DS3D_IMMEDIATE );
        if (hr == DS_OK)
            hr = IDirectSound3DListener_GetDistanceFactor( listener, &f );
        out_hr( "hr", hr );
        out( " bits=0x" ); out_hex( fbits( f ), 8 );
        out( " want=0x" ); out_hex( fbits( 2.5f ), 8 );
        verdict( hr == DS_OK && fbits( f ) == fbits( 2.5f ),
                 "a single by-value float did not survive the boundary" );

        /* Three floats in XMM1..XMM3, the DWORD on the stack. */
        begin( "3D position round-trip (3 floats)" );
        zero( &v, sizeof(v) );
        hr = IDirectSound3DListener_SetPosition( listener, 1.5f, -2.25f,
                                                  3.125f, DS3D_IMMEDIATE );
        if (hr == DS_OK) hr = IDirectSound3DListener_GetPosition( listener, &v );
        out_hr( "hr", hr );
        out( " x=0x" ); out_hex( fbits( v.x ), 8 );
        out( " y=0x" ); out_hex( fbits( v.y ), 8 );
        out( " z=0x" ); out_hex( fbits( v.z ), 8 );
        verdict( hr == DS_OK && fbits( v.x ) == fbits( 1.5f ) &&
                 fbits( v.y ) == fbits( -2.25f ) &&
                 fbits( v.z ) == fbits( 3.125f ),
                 "three by-value floats did not survive the boundary" );

        /* SIX floats: the fourth, fifth and sixth are past XMM3 and travel on
         * the guest's stack, which is the case a host that read every float
         * out of an XMM register gets WRONG rather than crashing on. */
        begin( "3D orientation round-trip (6 floats, 3 past XMM3)" );
        zero( &front, sizeof(front) );
        zero( &top, sizeof(top) );
        hr = IDirectSound3DListener_SetOrientation( listener,
                 0.5f, -0.25f, 0.125f, -0.0625f, 0.03125f, -0.015625f,
                 DS3D_IMMEDIATE );
        if (hr == DS_OK)
            hr = IDirectSound3DListener_GetOrientation( listener, &front, &top );
        out_hr( "hr", hr );
        out( " f=0x" ); out_hex( fbits( front.x ), 8 );
        out( ",0x" ); out_hex( fbits( front.y ), 8 );
        out( ",0x" ); out_hex( fbits( front.z ), 8 );
        out( " t=0x" ); out_hex( fbits( top.x ), 8 );
        out( ",0x" ); out_hex( fbits( top.y ), 8 );
        out( ",0x" ); out_hex( fbits( top.z ), 8 );
#if DS_SMOKE_BREAK == 3
        verdict( hr == DS_OK && fbits( front.x ) == fbits( 0.5f ) &&
                 fbits( top.z ) == fbits( 99.0f ),
                 "the six-float check is wired to its own expectation" );
#else
        verdict( hr == DS_OK &&
                 fbits( front.x ) == fbits( 0.5f ) &&
                 fbits( front.y ) == fbits( -0.25f ) &&
                 fbits( front.z ) == fbits( 0.125f ) &&
                 fbits( top.x )   == fbits( -0.0625f ) &&
                 fbits( top.y )   == fbits( 0.03125f ) &&
                 fbits( top.z )   == fbits( -0.015625f ),
                 "the fourth, fifth or sixth by-value float -- the ones that "
                 "travel on the stack -- did not arrive" );
#endif
    }

    /* ---- the port-specific observation, on stderr only ---------------- */
    {
        IDirectSoundBuffer *agg = NULL;
        IUnknown *outer = (IUnknown *)&desc;   /* any non-NULL pointer */

        hr = IDirectSound8_CreateSoundBuffer( ds, &desc, &agg, outer );
        note( "note: CreateSoundBuffer with a non-NULL pUnkOuter ->", hr );
        if (agg) IDirectSoundBuffer_Release( agg );
    }

    /* ---- 20: it all comes apart -------------------------------------- */
    {
        ULONG refs;

        begin( "release everything" );
        if (listener) IDirectSound3DListener_Release( listener );
        listener = NULL;
        if (primary) IDirectSoundBuffer_Release( primary );
        primary = NULL;
        if (notify) IDirectSoundNotify_Release( notify );
        notify = NULL;
        if (buf8) IDirectSoundBuffer8_Release( buf8 );
        buf8 = NULL;
        if (buf) IDirectSoundBuffer_Release( buf );
        buf = NULL;
        refs = IDirectSound8_Release( ds );
        ds = NULL;
        out( "final_refs=" ); out_dec( refs );
        verdict( refs == 0, "the device did not reach zero references" );
    }

done:
    if (listener) IDirectSound3DListener_Release( listener );
    if (primary) IDirectSoundBuffer_Release( primary );
    if (notify) IDirectSoundNotify_Release( notify );
    if (buf8) IDirectSoundBuffer8_Release( buf8 );
    if (buf) IDirectSoundBuffer_Release( buf );
    if (ds) IDirectSound8_Release( ds );
    if (ev) CloseHandle( ev );

    out( failures ? "ds_smoke: FAIL " : "ds_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    if (failures && first_fail)
    {
        out( " (" );
        out( first_fail );
        out( ")" );
    }
    out( "\n" );
    return failures ? 1 : 0;
}

#if defined(DS_SMOKE_NATIVE)
/* The native ppc64 PE leg: winegcc links a CRT, so this is an ordinary main. */
int main( void )
{
    return ds_smoke_run();
}
#else
/* The guest leg has no C runtime: this IS the image entry point. */
void WINAPI ds_smoke_entry( void )
{
    ExitProcess( (UINT)ds_smoke_run() );
}
#endif
