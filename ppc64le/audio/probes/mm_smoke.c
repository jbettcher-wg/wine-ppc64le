/*
 * mm_smoke -- the native-vs-guest WINMM audit.
 *
 * winmm already had a 187-export guest thunk before any of this work; what it
 * did not have was anyone checking that a guest which uses it gets SOUND
 * rather than a return code.  This is that check, in the same shape as
 * ds_smoke.c and xa_smoke.c: one source, built twice, run twice under the same
 * wine, and the two transcripts must be byte-identical.
 *
 * winmm is a FLAT surface -- no COM, no proxies -- so what is under test here
 * is the ordinary thunk path plus three things that are not ordinary:
 *
 *   * A STRUCT THE IMPLEMENTATION WRITES THROUGH, twice.  A WAVEHDR is handed
 *     to waveOutPrepareHeader, which fills in dwFlags and a private reserved
 *     field, and then to waveOutWrite, which the mixer completes
 *     asynchronously by setting WHDR_DONE in the guest's own memory.  Step 5
 *     waits for that bit: it is set by a NATIVE thread writing into a
 *     structure that lives inside the guest image.
 *   * A HANDLE CROSSING IN BOTH DIRECTIONS.  Step 7 opens the device with
 *     CALLBACK_EVENT, so winmm signals an event the guest created and the
 *     guest waits on it.
 *   * PlaySound with SND_MEMORY, i.e. a whole RIFF/WAVE image the guest built
 *     in its own memory and native code parses and plays.
 *
 * WHAT IS DELIBERATELY NOT HERE: waveOutOpen with CALLBACK_FUNCTION.  Its
 * callback arrives as a `DWORD_PTR dwCallback` whose meaning depends on a
 * FLAG in a later argument, so the port's callback interception -- which
 * classifies an argument by its declared TYPE and swaps it for a trampoline at
 * registration -- cannot see it, and a guest function pointer would reach
 * winmm as a raw address for a native thread to call.  That is a real gap and
 * it is recorded as one; a probe that exercised it would be measuring a crash.
 * CALLBACK_EVENT and CALLBACK_NULL, which is what most titles use, are fully
 * covered above.
 *
 * MM_SMOKE_BREAK (falsification):
 *
 *   =1  do not write the PCM, so the checksum step goes red against the
 *       expected value pcm_expected() computes from the generator.
 *   =2  never call waveOutWrite, so nothing is ever played and the position
 *       never advances.
 *
 * NO C RUNTIME on the guest leg; the program formats its own output.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MM_SMOKE_BREAK
#define MM_SMOKE_BREAK 0
#endif

#include <windows.h>
#include <mmsystem.h>

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

#define SAMPLE_RATE  44100
#define CHANNELS     2
#define BLOCK_ALIGN  (CHANNELS * 2)
#define PCM_FRAMES   (SAMPLE_RATE / 10)          /* 100ms */
#define PCM_BYTES    (PCM_FRAMES * BLOCK_ALIGN)

/* A whole RIFF/WAVE image, built here so PlaySound(SND_MEMORY) has something
 * of the guest's own to parse.  Little-endian on both sides of this boundary,
 * so the fields are written a byte at a time and there is nothing to swap. */
static BYTE wav[44 + PCM_BYTES];
static BYTE pcm[PCM_BYTES];

/* The checksum the payload MUST have, computed from the expression that fills
 * it rather than from the filled buffer.  Written out a second time on
 * purpose: an oracle that reads the thing it is checking is not an oracle, and
 * MM_SMOKE_BREAK=1 exists to prove this one is. */
static DWORD pcm_expected( void )
{
    DWORD hash = 0x811c9dc5u;
    UINT i;

    for (i = 0; i < PCM_BYTES; i++)
    {
        hash ^= (BYTE)(((i * 2654435761u) >> 13) ^ (i * 131u));
        hash *= 0x01000193u;
    }
    return hash;
}

static void put32( BYTE *p, ULONG v )
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8);
    p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24);
}

static void put16( BYTE *p, UINT v )
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8);
}

static void tag( BYTE *p, const char *s )
{
    p[0] = (BYTE)s[0]; p[1] = (BYTE)s[1]; p[2] = (BYTE)s[2]; p[3] = (BYTE)s[3];
}

static int mm_smoke_run( void )
{
    WAVEFORMATEX wfx;
    WAVEOUTCAPSW caps;
    HWAVEOUT hwo = NULL;
    WAVEHDR hdr;
    HANDLE ev = NULL;
    MMRESULT mr;
    UINT i, devs;

    for (i = 0; i < PCM_BYTES; i++)
#if MM_SMOKE_BREAK == 1
        pcm[i] = 0;
#else
        pcm[i] = (BYTE)(((i * 2654435761u) >> 13) ^ (i * 131u));
#endif

    zero( &wfx, sizeof(wfx) );
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = BLOCK_ALIGN;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * BLOCK_ALIGN;

    /* ---- 1: there is a device ---------------------------------------- */
    begin( "waveOutGetNumDevs" );
    devs = waveOutGetNumDevs();
    out( "devs>0=" ); out( devs > 0 ? "yes" : "no" );
    verdict( devs > 0, "winmm sees no output device" );

    /* ---- 2: and it describes itself ----------------------------------- */
    begin( "waveOutGetDevCapsW" );
    zero( &caps, sizeof(caps) );
    mr = waveOutGetDevCapsW( WAVE_MAPPER, &caps, sizeof(caps) );
    out( "mr=" ); out_dec( mr );
    out( " ch=" ); out_dec( caps.wChannels );
    out( " formats_set=" ); out( caps.dwFormats ? "yes" : "no" );
    out( " named=" ); out( caps.szPname[0] ? "yes" : "no" );
    verdict( mr == MMSYSERR_NOERROR && caps.wChannels > 0 && caps.dwFormats &&
             caps.szPname[0],
             "the wave mapper did not describe a device" );

    /* ---- 3: open ------------------------------------------------------ */
    begin( "waveOutOpen (CALLBACK_NULL)" );
    mr = waveOutOpen( &hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL );
    out( "mr=" ); out_dec( mr );
    out( " handle=" ); out( hwo ? "yes" : "no" );
    verdict( mr == MMSYSERR_NOERROR && hwo != NULL, "the device did not open" );
    if (!hwo) goto done;

    /* ---- 4-5: prepare, write, and the mixer completing our header ------ */
    begin( "waveOutPrepareHeader" );
    zero( &hdr, sizeof(hdr) );
    hdr.lpData         = (LPSTR)pcm;
    hdr.dwBufferLength = PCM_BYTES;
    mr = waveOutPrepareHeader( hwo, &hdr, sizeof(hdr) );
    out( "mr=" ); out_dec( mr );
    out( " prepared=" ); out( (hdr.dwFlags & WHDR_PREPARED) ? "yes" : "no" );
    verdict( mr == MMSYSERR_NOERROR && (hdr.dwFlags & WHDR_PREPARED),
             "the header was not marked prepared in the caller's own memory" );

    {
        DWORD start;
        BOOL done_bit = FALSE;

        begin( "waveOutWrite, and WHDR_DONE set by the mixer thread" );
#if MM_SMOKE_BREAK != 2
        mr = waveOutWrite( hwo, &hdr, sizeof(hdr) );
#else
        mr = MMSYSERR_NOERROR;
#endif
        start = GetTickCount();
        while (mr == MMSYSERR_NOERROR && GetTickCount() - start < 3000)
        {
            if (hdr.dwFlags & WHDR_DONE) { done_bit = TRUE; break; }
            Sleep( 10 );
        }
        out( "mr=" ); out_dec( mr );
        out( " done=" ); out( done_bit ? "yes" : "no" );
        verdict( mr == MMSYSERR_NOERROR && done_bit,
                 "WHDR_DONE was never set -- nothing played the buffer" );
    }

    /* ---- 6: the position moved, and the bytes we sent are still ours --- */
    {
        MMTIME t;
        DWORD sum = fnv1a( 0x811c9dc5u, pcm, PCM_BYTES );

        begin( "waveOutGetPosition and the payload checksum" );
        zero( &t, sizeof(t) );
        t.wType = TIME_BYTES;
        mr = waveOutGetPosition( hwo, &t, sizeof(t) );
        out( "mr=" ); out_dec( mr );
        out( " type=" ); out_dec( t.wType );
        out( " advanced=" ); out( t.u.cb > 0 ? "yes" : "no" );
        out( " pcm=0x" ); out_hex( sum, 8 );
        /* THE ORACLE IS THE GENERATOR, not the buffer.  This check used to
         * compare fnv1a(pcm) against fnv1a(hdr.lpData) -- and hdr.lpData IS
         * pcm, so it compared the buffer against itself and could not fail.
         * MM_SMOKE_BREAK=1 zeroes the payload, which changed both sides
         * identically and PASSED: a control that cannot go red, which is the
         * one thing this tree does not accept.
         *
         * pcm_expected() recomputes the checksum from the same expression that
         * FILLED the buffer, in a function the break does not touch, so what
         * is compared is "the bytes are the ones we generated" -- which is what
         * the step always claimed and never checked.  hdr.lpData is still
         * required to be the buffer, because that is a separate fact worth
         * having and it is now stated separately. */
        verdict( mr == MMSYSERR_NOERROR && t.wType == TIME_BYTES && t.u.cb > 0 &&
                 (const BYTE *)hdr.lpData == pcm &&
                 hdr.dwBufferLength == PCM_BYTES &&
                 sum == pcm_expected(),
                 "the device played nothing, or the payload is not the one this "
                 "program generated" );
    }

    begin( "waveOutUnprepareHeader and waveOutClose" );
    mr = waveOutUnprepareHeader( hwo, &hdr, sizeof(hdr) );
    if (mr == MMSYSERR_NOERROR) mr = waveOutClose( hwo );
    hwo = NULL;
    out( "mr=" ); out_dec( mr );
    verdict( mr == MMSYSERR_NOERROR, "the device did not come apart cleanly" );

    /* ---- 7: CALLBACK_EVENT -- winmm signals a handle the guest made ---- */
    {
        DWORD waited;

        begin( "waveOutOpen (CALLBACK_EVENT), and the event is signalled" );
        ev = CreateEventW( NULL, FALSE, FALSE, NULL );
        mr = waveOutOpen( &hwo, WAVE_MAPPER, &wfx, (DWORD_PTR)ev, 0,
                          CALLBACK_EVENT );
        waited = (mr == MMSYSERR_NOERROR) ? WaitForSingleObject( ev, 3000 )
                                          : WAIT_FAILED;
        out( "mr=" ); out_dec( mr );
        out( " signalled=" ); out( waited == WAIT_OBJECT_0 ? "yes" : "no" );
        verdict( mr == MMSYSERR_NOERROR && waited == WAIT_OBJECT_0,
                 "winmm never signalled the guest's own event handle" );
        if (hwo) { waveOutReset( hwo ); waveOutClose( hwo ); hwo = NULL; }
    }

    /* ---- 8: PlaySound with a whole RIFF image of the guest's own ------- */
    {
        BOOL ok;

        begin( "PlaySoundW SND_MEMORY|SND_SYNC" );
        tag( wav + 0,  "RIFF" );
        put32( wav + 4, 36 + PCM_BYTES );
        tag( wav + 8,  "WAVE" );
        tag( wav + 12, "fmt " );
        put32( wav + 16, 16 );
        put16( wav + 20, WAVE_FORMAT_PCM );
        put16( wav + 22, CHANNELS );
        put32( wav + 24, SAMPLE_RATE );
        put32( wav + 28, SAMPLE_RATE * BLOCK_ALIGN );
        put16( wav + 32, BLOCK_ALIGN );
        put16( wav + 34, 16 );
        tag( wav + 36, "data" );
        put32( wav + 40, PCM_BYTES );
        for (i = 0; i < PCM_BYTES; i++) wav[44 + i] = pcm[i];

        ok = PlaySoundW( (const WCHAR *)wav, NULL, SND_MEMORY | SND_SYNC |
                         SND_NODEFAULT );
        out( "played=" ); out( ok ? "yes" : "no" );
        verdict( ok, "PlaySound refused an in-memory RIFF image" );
        PlaySoundW( NULL, NULL, SND_PURGE );
    }

done:
    if (hwo) { waveOutReset( hwo ); waveOutClose( hwo ); }
    if (ev) CloseHandle( ev );

    out( failures ? "mm_smoke: FAIL " : "mm_smoke: PASS " );
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

#if defined(MM_SMOKE_NATIVE)
int main( void )
{
    return mm_smoke_run();
}
#else
void WINAPI mm_smoke_entry( void )
{
    ExitProcess( (UINT)mm_smoke_run() );
}
#endif
