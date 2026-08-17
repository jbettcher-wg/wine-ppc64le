/*
 * shell_smoke -- the everyday-DLL guest surface gate.
 *
 * ONE source, built TWICE and run twice: as a native ppc64 Windows PE and as
 * an x86-64 guest PE.  The two runs must print BYTE-IDENTICAL output.  That is
 * the bar rather than "the guest said PASS" for the reason
 * ppc64le/syscom/check-com-smoke.sh gives at length: reaching the right answer
 * through the wrong mechanism is exactly the failure a PASS/PASS comparison
 * cannot see.  Every byte printed below is a value Wine's own implementation
 * computed, or a constant this program checked against arithmetic it did
 * itself.
 *
 * FOUR MODULES, chosen because each is a different KIND of boundary:
 *
 *   dinput8   COM.  DirectInput8Create vends an IDirectInput8A, whose vtable
 *             on the guest side is a block of x86-64 trap stubs published by
 *             the guest thunk module and dispatched by native dinput8's
 *             __wine_com_dispatch (dlls/dinput8/guestcom.c).  The probe walks
 *             the path a game actually walks -- CreateDevice(GUID_SysKeyboard),
 *             SetDataFormat, SetCooperativeLevel, Acquire, GetDeviceState --
 *             and VALUE-CHECKS the result: with no key held, all 256 bytes of
 *             the keyboard state must be zero, and the call must have written
 *             exactly 256 bytes and not one more.  A poisoned buffer proves
 *             the write happened and landed where it was asked to.
 *
 *   d3dx9_42  A FLAT surface with floating point.  D3DXMatrixMultiply and
 *             D3DXVec3Normalize are checked against arithmetic this file does
 *             itself, bit for bit -- the results are printed as raw IEEE
 *             words, so a float that travelled in the wrong register file is
 *             a different hex string and not a rounding argument.
 *
 *   hid       A flat surface with a KNOWN ANSWER.  HidD_GetHidGuid writes the
 *             HID class GUID, which is a fixed constant on every Windows and
 *             is spelled out below; if the out-pointer did not cross
 *             correctly the bytes are not that GUID.
 *
 *   comctl32  WINDOW MACHINERY, and GUEST CODE HANDED TO NATIVE CODE.
 *             InitCommonControlsEx registers the control classes, and the
 *             probe proves they exist by asking user32 for one of them by
 *             name -- GetClassInfoExA("SysListView32") -- and printing the
 *             registered class's own cbWndExtra and style.  That is a value
 *             comctl32 chose, read back through user32, so it cannot be
 *             produced by a call that quietly did nothing.
 *
 *             Then the two callback shapes.  DPA_Sort/DPA_Search/
 *             DPA_EnumCallback/DPA_DestroyCallback/DSA_DestroyCallback hand
 *             native comctl32 a bare comparator or enumerator; the probe
 *             sorts a KNOWN scrambled array with one and prints the resulting
 *             ORDER, which is a permutation only a comparator that really ran
 *             -- on the right elements, in the right argument order -- can
 *             produce.  CreatePropertySheetPage and PropertySheet carry
 *             pfnDlgProc and pfnCallback INSIDE a struct; the probe builds an
 *             in-memory dialog template, puts up a MODELESS property sheet and
 *             prints which callback codes arrived and whether the page's own
 *             lParam came back through each of them intact.
 *
 * IDirectInput8::EnumDevices HAS ITS OWN LEG, and what that leg claims has
 * just inverted.  It used to be REFUSED on the guest side by name, because it
 * hands native dinput a bare guest FUNCTION POINTER that dinput retains and
 * calls once per device from a native frame.  dlls/dinput8/guestcom.c now
 * serves it: the guest pointer is swapped, at the moment it arrives, for one
 * of ntdll's guest-callback trampolines (__wine_guest_wrap_callback), which is
 * the port's answer for a bare callback and is NOT a reverse proxy -- a
 * reverse proxy is a vtable, and a DIENUMDEVICESCALLBACK has none.
 *
 * So the leg now requires the two runs to AGREE: the same HRESULT and the same
 * device count, with the callback entered once per device in both.  It stays a
 * separate build (-DSHELL_SMOKE_ENUM) rather than joining the identity leg,
 * because how many devices this machine has is not a property of the boundary
 * and the main leg should not depend on it.
 *
 * IDirectInput8::EnumDevices AND GetOpenFileName-WITH-A-HOOK each have their
 * own build, for the same reason: what they print depends on the machine (how
 * many input devices it has) or on a dialog that has to be cancelled, and the
 * main identity leg should not carry either.  Both are still compared native
 * against guest, byte for byte.
 *
 *   -DSHELL_SMOKE_ENUM  IDirectInput8A::EnumDevices, whose callback is a bare
 *                       guest function pointer native dinput retains and calls
 *                       once per device from a native frame.  The probe VALUE
 *                       CHECKS what the callback received: dwSize, a non-null
 *                       instance GUID, a non-zero device type and a non-empty
 *                       instance name, folded into a digest both runs must
 *                       agree on.
 *   -DSHELL_SMOKE_HOOK  GetOpenFileNameA with OFN_ENABLEHOOK, which is the
 *                       callback-inside-a-struct shape.  The hook records that
 *                       it was entered, checks that the OPENFILENAME it was
 *                       handed is THE CALLER'S OWN (its lCustData is the
 *                       probe's cookie), and cancels the dialog so the run
 *                       terminates without anybody touching a keyboard.
 *
 * NO C RUNTIME on the guest side (-DSHELL_SMOKE_NO_CRT): the program formats
 * its own output and writes it with WriteFile, exactly as com_smoke.c does
 * and for the same reason -- a CRT would add a second variable to a test
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
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <prsht.h>
#include <commdlg.h>
#include <dinput.h>
#include <d3dx9.h>
#include <ddk/hidsdi.h>

/* Spelled out here rather than linked from libdxguid: the guest build has no
 * Wine import libraries at all, and a GUID both builds compile from the same
 * bytes cannot differ between them. */
static const GUID smoke_IID_IDirectInput8A =
    { 0xbf798030, 0x483a, 0x4da2, { 0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00 } };
static const GUID smoke_GUID_SysKeyboard =
    { 0x6f1d2b61, 0xd5a0, 0x11cf, { 0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00 } };
static const GUID smoke_GUID_Key =
    { 0x55728220, 0xd33c, 0x11cf, { 0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00 } };
/* GUID_DEVINTERFACE_HID -- the HID class interface, the same constant on
 * every Windows since 2000 and what HidD_GetHidGuid must write. */
static const GUID smoke_GUID_HID =
    { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30 } };

/* SHELL_SMOKE_BREAK=<n> is the gate's negative control on the VALUE CHECKS
 * themselves.  A gate whose comparisons cannot go red proves nothing, so the
 * NATIVE leg is rebuilt with each of these and must FAIL:
 *
 *   1  the keyboard data format claims 255 bytes of state, which dinput must
 *      reject -- proving SetDataFormat is really validated and not nodded at
 *   2  the expected HID class GUID is off by one byte -- proving the GUID
 *      comparison is a comparison
 *   3  the expected matrix product is off by one -- proving the
 *      floating-point check is a check
 *   4  the expected DPA_Sort order is off by one -- proving the comparator's
 *      result is really compared and not merely printed
 *   5  the expected property-sheet callback mask is off by one -- proving the
 *      "which callbacks arrived" check is a check
 *
 * Nothing under these is reachable in an ordinary build.
 */
#ifndef SHELL_SMOKE_BREAK
#define SHELL_SMOKE_BREAK 0
#endif

/* ------------------------------------------------------------- output */

static void out( const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, n, &written, NULL );
}

static void out_hex( ULONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
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

static void out_hr( const char *label, HRESULT hr )
{
    out( label );
    out( "=0x" );
    out_hex( (ULONG)hr, 8 );
}

static void out_guid( const GUID *g )
{
    int i;

    out( "{" );
    out_hex( g->Data1, 8 );
    out( "-" );
    out_hex( g->Data2, 4 );
    out( "-" );
    out_hex( g->Data3, 4 );
    out( "-" );
    for (i = 0; i < 8; i++)
    {
        if (i == 2) out( "-" );
        out_hex( g->Data4[i], 2 );
    }
    out( "}" );
}

static BOOL guid_eq( const GUID *a, const GUID *b )
{
    const BYTE *p = (const BYTE *)a, *q = (const BYTE *)b;
    int i;

    for (i = 0; i < (int)sizeof(GUID); i++) if (p[i] != q[i]) return FALSE;
    return TRUE;
}

/* Floats are printed as their raw IEEE words.  A float that arrived through
 * the wrong register file, or was truncated from a double, is then a
 * different hex string rather than a rounding argument. */
static ULONG f_bits( float f )
{
    union { float f; ULONG u; } u;
    u.f = f;
    return u.u;
}

/* ------------------------------------------------------------- the run */

static int failures;
static int step;

static void begin( const char *what )
{
    out( "step " );
    out_dec( ++step );
    out( " " );
    out( what );
    out( ": " );
}

static void verdict( BOOL ok, const char *why )
{
    if (ok) out( " ok\n" );
    else
    {
        failures++;
        out( " FAIL (" );
        out( why );
        out( ")\n" );
    }
}

/* The keyboard data format, built here rather than taken from the
 * c_dfDIKeyboard the import library would supply: the guest build has no Wine
 * import libraries, and a table both builds construct with the same loop
 * cannot differ between them.  This is byte-for-byte what
 * dlls/dinput/data_formats.c defines. */
static DIOBJECTDATAFORMAT smoke_keyboard_objs[256];
static DIDATAFORMAT smoke_keyboard_format;

static void build_keyboard_format( void )
{
    int i;

    for (i = 0; i < 256; i++)
    {
        smoke_keyboard_objs[i].pguid  = &smoke_GUID_Key;
        smoke_keyboard_objs[i].dwOfs  = i;
        smoke_keyboard_objs[i].dwType = DIDFT_OPTIONAL | DIDFT_BUTTON
                                        | DIDFT_MAKEINSTANCE(i);
        smoke_keyboard_objs[i].dwFlags = 0;
    }
    smoke_keyboard_format.dwSize     = sizeof(smoke_keyboard_format);
    smoke_keyboard_format.dwObjSize  = sizeof(smoke_keyboard_objs[0]);
    smoke_keyboard_format.dwFlags    = DIDF_RELAXIS;
#if SHELL_SMOKE_BREAK == 1
    smoke_keyboard_format.dwDataSize = 255;   /* negative control */
#else
    smoke_keyboard_format.dwDataSize = 256;
#endif
    smoke_keyboard_format.dwNumObjs  = 256;
    smoke_keyboard_format.rgodf      = smoke_keyboard_objs;
}

#ifdef SHELL_SMOKE_ENUM
/* Set once per entry: the callback's OWN first argument is a
 * DIDEVICEINSTANCEA the native side filled in, and a trampoline carries it
 * through untranslated -- which is correct here and only here, because both
 * sides compile that struct from this same header and guest memory IS host
 * memory.  Checking dwSize is what makes "the pointer arrived" a fact rather
 * than an assumption. */
static ULONG smoke_enum_ref_ok = 1;

/* Set to 0 if a call made FROM INSIDE the callback did not work.  This is the
 * shape a real game has and the one a counter-only callback does not test: an
 * enumeration callback that only counts proves the trampoline was entered,
 * while a game's callback immediately calls back into the API -- reads a tick
 * count, creates the device it just found -- and every one of those is a
 * guest->native trap raised from inside a NESTED emulator run that native code
 * started.  "The callback was entered" and "the callback can do its job" are
 * different claims. */
static ULONG smoke_enum_call_ok = 1;

/* WHAT THE CALLBACK IS ENTITLED TO SEE, and it is every field.  The struct is
 * one the NATIVE side filled in, and a trampoline carries it through
 * untranslated -- correct here and only here, because both sides compile
 * DIDEVICEINSTANCEA from this same header and guest memory IS host memory.
 * So:
 *
 *   dwSize           exactly sizeof(DIDEVICEINSTANCEA): a struct that crossed
 *                    at the wrong offset does not have this
 *   guidInstance     not all-zero: dinput WROTE it
 *   dwDevType        not zero: every device has a class
 *   tszInstanceName  a NUL-terminated, non-empty string INSIDE its own array
 *                    -- the field furthest from the head of the struct this
 *                    probe can judge, so it also says the whole body arrived
 *                    and not just the first eight bytes
 *
 * The DIGEST folds those fields of every device into one word.  Both runs
 * enumerate the SAME machine, so it is a value they must agree on, and it is
 * what turns "the callback ran N times" into "the callback saw the same N
 * devices, field for field".  A count alone cannot tell those two apart. */
/* All-zero, so that "dinput WROTE this GUID" is a comparison rather than a
 * hope.  Inside this leg's #ifdef because it is the only thing that uses it,
 * and an unused constant is a warning, and a probe that builds with warnings
 * is not evidence. */
static const GUID smoke_GUID_null =
    { 0, 0, 0, { 0,0,0,0,0,0,0,0 } };

static ULONG smoke_enum_digest = 2166136261u;      /* FNV-1a basis */

static void smoke_enum_fold( const void *p, int n )
{
    const BYTE *b = p;
    int i;

    for (i = 0; i < n; i++)
    {
        smoke_enum_digest ^= b[i];
        smoke_enum_digest *= 16777619u;
    }
}

static BOOL CALLBACK smoke_enum_cb( LPCDIDEVICEINSTANCEA inst, LPVOID ref )
{
    DWORD tick, tid;
    int n = 0;

    if (!inst || inst->dwSize != sizeof(DIDEVICEINSTANCEA)) smoke_enum_ref_ok = 0;

    /* A flat thunked call, from inside the callback. */
    tid  = GetCurrentThreadId();
    tick = GetTickCount();
    if (!tid || !tick) smoke_enum_call_ok = 0;

    if (inst && inst->dwSize == sizeof(DIDEVICEINSTANCEA))
    {
        if (!inst->dwDevType ||
            guid_eq( &inst->guidInstance, &smoke_GUID_null )) smoke_enum_ref_ok = 0;
        while (n < (int)sizeof(inst->tszInstanceName) && inst->tszInstanceName[n]) n++;
        if (!n || n == (int)sizeof(inst->tszInstanceName)) smoke_enum_ref_ok = 0;
        smoke_enum_fold( &inst->guidInstance, sizeof(GUID) );
        smoke_enum_fold( &inst->guidProduct, sizeof(GUID) );
        smoke_enum_fold( &inst->dwDevType, sizeof(DWORD) );
        smoke_enum_fold( inst->tszInstanceName, n );
    }

    (*(ULONG *)ref)++;
    return DIENUM_CONTINUE;
}
#endif

static HWND smoke_window( void )
{
    WNDCLASSEXA wc;
    HWND hwnd;

    /* memset by hand: no CRT on the guest side. */
    { BYTE *p = (BYTE *)&wc; int i; for (i = 0; i < (int)sizeof(wc); i++) p[i] = 0; }
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA( NULL );
    wc.lpszClassName = "shell_smoke_window";
    if (!RegisterClassExA( &wc )) return NULL;
    /* HWND_MESSAGE: a message-only window.  It is never mapped, so nothing
     * this gate does can appear on anybody's screen, and DirectInput's
     * DISCL_BACKGROUND cooperative level accepts it. */
    hwnd = CreateWindowExA( 0, "shell_smoke_window", "shell_smoke", 0,
                            0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL );
    return hwnd;
}

static void run_dinput8( HWND hwnd )
{
    IDirectInputDevice8A *dev = NULL;
    IDirectInput8A *di = NULL;
    BYTE keys[256 + 8];
    HRESULT hr;
    int i, nonzero = 0, past = 0;

    begin( "DirectInput8Create(IID_IDirectInput8A)" );
    hr = DirectInput8Create( GetModuleHandleA( NULL ), DIRECTINPUT_VERSION,
                             &smoke_IID_IDirectInput8A, (void **)&di, NULL );
    out_hr( "hr", hr );
    verdict( hr == DI_OK && di != NULL, "no IDirectInput8A" );
    if (hr != DI_OK || !di) return;

    begin( "IDirectInput8A::CreateDevice(GUID_SysKeyboard)" );
    hr = IDirectInput8_CreateDevice( di, &smoke_GUID_SysKeyboard, &dev, NULL );
    out_hr( "hr", hr );
    verdict( hr == DI_OK && dev != NULL, "no device" );
    if (hr != DI_OK || !dev) goto release_di;

    begin( "IDirectInputDevice8A::SetDataFormat(keyboard, 256 objects)" );
    build_keyboard_format();
    hr = IDirectInputDevice8_SetDataFormat( dev, &smoke_keyboard_format );
    out_hr( "hr", hr );
    verdict( hr == DI_OK, "not DI_OK" );

    begin( "IDirectInputDevice8A::SetCooperativeLevel(BACKGROUND|NONEXCLUSIVE)" );
    hr = IDirectInputDevice8_SetCooperativeLevel(
             dev, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE );
    out_hr( "hr", hr );
    verdict( hr == DI_OK, "not DI_OK" );

    begin( "IDirectInputDevice8A::Acquire" );
    hr = IDirectInputDevice8_Acquire( dev );
    out_hr( "hr", hr );
    verdict( hr == DI_OK || hr == S_FALSE, "not acquired" );

    /* THE ONE THAT MATTERS: the polling call a game makes every frame.  The
     * buffer is poisoned first and eight bytes past the end are poisoned too,
     * so "all 256 are zero" proves the call WROTE them and "the guard is
     * intact" proves it wrote exactly 256.  With no key held, every one of
     * those 256 bytes must be zero -- that is the DirectInput contract, and
     * it is a value check rather than an is-it-still-alive check. */
    begin( "IDirectInputDevice8A::GetDeviceState(256)" );
    for (i = 0; i < (int)sizeof(keys); i++) keys[i] = 0xA5;
    hr = IDirectInputDevice8_GetDeviceState( dev, 256, keys );
    for (i = 0; i < 256; i++) if (keys[i]) nonzero++;
    for (i = 256; i < (int)sizeof(keys); i++) if (keys[i] != 0xA5) past++;
    out_hr( "hr", hr );
    out( " nonzero=" );
    out_dec( nonzero );
    out( " past_end=" );
    out_dec( past );
    verdict( hr == DI_OK && nonzero == 0 && past == 0,
             "state not written as exactly 256 zero bytes" );

    /* A wrong size must be REFUSED by dinput and not merely tolerated: this
     * is the call proving the DWORD argument crossed with its real value and
     * was not, say, truncated or read from the wrong slot. */
    begin( "IDirectInputDevice8A::GetDeviceState(255) is rejected" );
    hr = IDirectInputDevice8_GetDeviceState( dev, 255, keys );
    out_hr( "hr", hr );
    verdict( hr == DIERR_INVALIDPARAM, "not DIERR_INVALIDPARAM" );

    begin( "IDirectInputDevice8A::Poll" );
    hr = IDirectInputDevice8_Poll( dev );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "poll failed" );

    begin( "IDirectInputDevice8A::Unacquire" );
    hr = IDirectInputDevice8_Unacquire( dev );
    out_hr( "hr", hr );
    verdict( SUCCEEDED(hr), "not released" );

#ifdef SHELL_SMOKE_ENUM
    /* The refusal leg.  Compiled in only for the gate's dedicated run,
     * because the two builds MUST disagree here: on the guest side this is
     * E_NOTIMPL from winecom's refuse row, on the native side it is a real
     * enumeration.  Printing it in the identity legs would be comparing the
     * boundary against itself. */
    {
        ULONG seen = 0;
        begin( "IDirectInput8A::EnumDevices(DI8DEVCLASS_ALL)" );
        hr = IDirectInput8_EnumDevices( di, DI8DEVCLASS_ALL, smoke_enum_cb,
                                        &seen, DIEDFL_ALLDEVICES );
        out_hr( "hr", hr );
        /* The device COUNT, and it is the load-bearing half of this leg: an
         * EnumDevices that returned S_OK without ever entering the callback
         * would print the same HRESULT as one that worked.  `seen` is
         * incremented through the pvRef the caller passed, so a non-zero count
         * also proves pvRef crossed untouched. */
        out( ", devices=" );
        out_dec( seen );
        out( ", cb_arg_ok=" );
        out_dec( smoke_enum_ref_ok );
        out( ", cb_call_ok=" );
        out_dec( smoke_enum_call_ok );
        out( ", digest=0x" );
        out_hex( smoke_enum_digest, 8 );
        verdict( TRUE, "" );
    }
#endif

    begin( "IDirectInputDevice8A::Release" );
    out_dec( IDirectInputDevice8_Release( dev ) );
    out( " refs left" );
    verdict( TRUE, "" );

release_di:
    begin( "IDirectInput8A::Release" );
    out_dec( IDirectInput8_Release( di ) );
    out( " refs left" );
    verdict( TRUE, "" );
}

static void run_d3dx9( void )
{
    D3DXVECTOR3 v = { 3.0f, 4.0f, 12.0f }, n;
    D3DXMATRIX a, b, m;
    int i, j, bad = 0;

    /* Two matrices whose product this file can state exactly: `a` scales by
     * (2,3,4) and translates by (5,6,7); `b` is the identity with a further
     * translation of (1,1,1).  Integer-valued floats throughout, so every
     * element of the product is exact in single precision and a bit-for-bit
     * comparison is the right comparison. */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) { a.m[i][j] = 0.0f; b.m[i][j] = 0.0f; }
    a.m[0][0] = 2.0f; a.m[1][1] = 3.0f; a.m[2][2] = 4.0f; a.m[3][3] = 1.0f;
    a.m[3][0] = 5.0f; a.m[3][1] = 6.0f; a.m[3][2] = 7.0f;
    b.m[0][0] = 1.0f; b.m[1][1] = 1.0f; b.m[2][2] = 1.0f; b.m[3][3] = 1.0f;
    b.m[3][0] = 1.0f; b.m[3][1] = 1.0f; b.m[3][2] = 1.0f;

    begin( "D3DXMatrixMultiply" );
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) m.m[i][j] = -1.0f;
    if (D3DXMatrixMultiply( &m, &a, &b ) != &m) bad++;
    out( "row3=" );
    for (j = 0; j < 3; j++) { out_hex( f_bits( m.m[3][j] ), 8 ); out( " " ); }
    out( "diag=" );
    for (i = 0; i < 3; i++) { out_hex( f_bits( m.m[i][i] ), 8 ); out( " " ); }
    if (m.m[0][0] != 2.0f || m.m[1][1] != 3.0f || m.m[2][2] != 4.0f) bad++;
#if SHELL_SMOKE_BREAK == 3
    if (m.m[3][0] != 66.0f) bad++;            /* negative control */
#else
    if (m.m[3][0] != 6.0f || m.m[3][1] != 7.0f || m.m[3][2] != 8.0f) bad++;
#endif
    verdict( bad == 0, "product is not scale(2,3,4) x translate(6,7,8)" );

    /* (3,4,12) has length exactly 13, so the normalised vector is
     * (3/13, 4/13, 12/13) -- not exact in binary, which is the point: the
     * bits are compared against the same division done here, so a float that
     * came back through a different path is a different word. */
    begin( "D3DXVec3Normalize" );
    n.x = n.y = n.z = -1.0f;
    if (D3DXVec3Normalize( &n, &v ) != &n) bad++;
    out( "n=" );
    out_hex( f_bits( n.x ), 8 ); out( " " );
    out_hex( f_bits( n.y ), 8 ); out( " " );
    out_hex( f_bits( n.z ), 8 );
    if (f_bits( n.x ) != f_bits( 3.0f / 13.0f ) ||
        f_bits( n.y ) != f_bits( 4.0f / 13.0f ) ||
        f_bits( n.z ) != f_bits( 12.0f / 13.0f )) bad++;
    verdict( bad == 0, "not (3,4,12)/13 bit for bit" );
}

static void run_hid( void )
{
    GUID g;

    begin( "HidD_GetHidGuid" );
    g = smoke_IID_IDirectInput8A;             /* a value it must overwrite */
    HidD_GetHidGuid( &g );
    out_guid( &g );
#if SHELL_SMOKE_BREAK == 2
    { GUID w = smoke_GUID_HID; w.Data4[7] ^= 1;   /* negative control */
      verdict( guid_eq( &g, &w ), "not GUID_DEVINTERFACE_HID" ); }
#else
    verdict( guid_eq( &g, &smoke_GUID_HID ), "not GUID_DEVINTERFACE_HID" );
#endif
}

static void run_comctl32( void )
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXA wc;
    BOOL ok;

    begin( "InitCommonControlsEx(ICC_LISTVIEW_CLASSES)" );
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    ok = InitCommonControlsEx( &icc );
    out( ok ? "TRUE" : "FALSE" );
    verdict( ok, "returned FALSE" );

    /* Prove the class REGISTRATION actually happened, by reading back a value
     * comctl32 itself chose.  A call that quietly did nothing cannot produce
     * this: GetClassInfoExA answers out of user32's class table.
     *
     * hInstance is NULL, deliberately, and not this module's or comctl32's.
     * comctl32 registers its controls with CS_GLOBALCLASS, so a NULL instance
     * finds them -- and asking for comctl32's own handle would compare two
     * DIFFERENT things in the two runs: GetModuleHandleA is intercepted for a
     * guest (emu_GetModuleHandleA in dlls/ntdll/signal_ppc64.c) and answers
     * the GUEST comctl32 thunk's base, while the class was registered by the
     * NATIVE module.  That is correct behaviour on both sides and would still
     * have broken the identity leg, which is the kind of thing this gate is
     * for noticing. */
    begin( "GetClassInfoExA(SysListView32)" );
    { BYTE *p = (BYTE *)&wc; int i; for (i = 0; i < (int)sizeof(wc); i++) p[i] = 0; }
    wc.cbSize = sizeof(wc);
    ok = GetClassInfoExA( NULL, "SysListView32", &wc );
    out( ok ? "TRUE" : "FALSE" );
    out( " style=0x" );
    out_hex( wc.style, 8 );
    out( " cbWndExtra=" );
    out_dec( (ULONG)wc.cbWndExtra );
    verdict( ok && wc.lpfnWndProc != NULL, "the class is not registered" );
}

/* ==================== comctl32: guest code, handed to native code =========
 *
 * Two shapes, and they are the two the port has to serve separately.
 *
 *   AS AN ARGUMENT.  DPA_Sort takes a comparator native comctl32 calls once
 *   per comparison.  This leg does not ask whether the call "worked": it
 *   prints the RESULTING ORDER of a known scrambled array, which is a
 *   permutation only a comparator that really ran -- on the right elements,
 *   with its two arguments the right way round -- can produce.  A comparator
 *   that was never entered leaves the input order; one entered with its
 *   arguments swapped produces the reverse.  Neither is what is printed.
 *
 *   INSIDE A STRUCT.  A PROPSHEETPAGE carries pfnDlgProc and pfnCallback, and
 *   there is no argument position that names either.  See run_propsheet.
 */

#define SMOKE_DPA_N       8
#define SMOKE_DPA_TOTAL   0x1DE     /* the sum of the eight below */
#define SMOKE_DSA_TOTAL   0xDB      /* the sum of the first three */

/* Used AS pointers, which is what a DPA stores.  Scrambled, distinct, and
 * chosen so that no two are adjacent -- an off-by-one in the comparator's
 * result cannot accidentally reproduce this order. */
static const ULONG smoke_dpa_in[SMOKE_DPA_N] =
    { 0x51, 0x13, 0x77, 0x22, 0x05, 0x60, 0x34, 0x48 };
static const ULONG smoke_dpa_sorted[SMOKE_DPA_N] =
    { 0x05, 0x13, 0x22, 0x34, 0x48, 0x51, 0x60, 0x77 };

/* A full sixty-four bits, deliberately: DPA_Sort's third argument is an LPARAM
 * the comparator receives untouched, and this is the one value in the leg that
 * would survive a boundary that quietly carried thirty-two. */
#define SMOKE_DPA_LPARAM  ((LPARAM)0x123456789ABCDEF0ull)

static ULONG smoke_dpa_lparam_ok = 1;
static ULONG smoke_dpa_ref_ok    = 1;
static ULONG smoke_dpa_calls;
static ULONG smoke_dpa_sum;
static ULONG smoke_dpa_destroyed;
static ULONG smoke_dsa_sum;
static ULONG smoke_dsa_destroyed;
static ULONG smoke_dpa_ctx;      /* its ADDRESS is the pvData every callback gets */

/* Returns -1 as well as +1, which is the case the NARROW trampoline exists
 * for: an ELFv2 caller reads a sign-extended 32-bit result, and a -1 that
 * arrived as 0x00000000ffffffff would sort the array backwards. */
static INT CALLBACK smoke_dpa_compare( LPVOID a, LPVOID b, LPARAM lp )
{
    smoke_dpa_calls++;
    if (lp != SMOKE_DPA_LPARAM) smoke_dpa_lparam_ok = 0;
    if ((ULONG_PTR)a < (ULONG_PTR)b) return -1;
    if ((ULONG_PTR)a > (ULONG_PTR)b) return 1;
    return 0;
}

static INT CALLBACK smoke_dpa_enum( LPVOID item, LPVOID ctx )
{
    if (ctx != &smoke_dpa_ctx) smoke_dpa_ref_ok = 0;
    smoke_dpa_sum += (ULONG)(ULONG_PTR)item;
    return 1;
}

static INT CALLBACK smoke_dpa_destroy( LPVOID item, LPVOID ctx )
{
    if (ctx != &smoke_dpa_ctx) smoke_dpa_ref_ok = 0;
    smoke_dpa_destroyed++;
    return 1;
}

static INT CALLBACK smoke_dsa_destroy( LPVOID item, LPVOID ctx )
{
    if (ctx != &smoke_dpa_ctx) smoke_dpa_ref_ok = 0;
    smoke_dsa_sum += *(const ULONG *)item;
    smoke_dsa_destroyed++;
    return 1;
}

static void run_comctl32_callbacks( void )
{
    ULONG order[SMOKE_DPA_N];
    int i, bad = 0, found, missing;
    HDPA hdpa;
    HDSA hdsa;

    begin( "DPA_Sort(guest comparator)" );
    hdpa = DPA_Create( SMOKE_DPA_N );
    for (i = 0; i < SMOKE_DPA_N; i++)
        if (DPA_InsertPtr( hdpa, DPA_APPEND,
                           (void *)(ULONG_PTR)smoke_dpa_in[i] ) < 0) bad++;
    if (!DPA_Sort( hdpa, smoke_dpa_compare, SMOKE_DPA_LPARAM )) bad++;
    out( "order=" );
    for (i = 0; i < SMOKE_DPA_N; i++)
    {
        order[i] = (ULONG)(ULONG_PTR)DPA_GetPtr( hdpa, i );
        out_hex( order[i], 2 );
        if (i + 1 < SMOKE_DPA_N) out( "," );
    }
    for (i = 0; i < SMOKE_DPA_N; i++)
#if SHELL_SMOKE_BREAK == 4
        if (order[i] != smoke_dpa_sorted[(i + 1) % SMOKE_DPA_N]) bad++;  /* control */
#else
        if (order[i] != smoke_dpa_sorted[i]) bad++;
#endif
    out( " entered=" );
    out_dec( smoke_dpa_calls ? 1 : 0 );
    out( " lparam_ok=" );
    out_dec( smoke_dpa_lparam_ok );
    verdict( bad == 0 && smoke_dpa_calls != 0 && smoke_dpa_lparam_ok,
             "the guest comparator did not sort the array" );

    /* A binary search over the sorted array, which reaches the comparator
     * through a different export and a different argument position. */
    begin( "DPA_Search(DPAS_SORTED, guest comparator)" );
    found   = DPA_Search( hdpa, (void *)(ULONG_PTR)0x34, 0, smoke_dpa_compare,
                          SMOKE_DPA_LPARAM, DPAS_SORTED );
    missing = DPA_Search( hdpa, (void *)(ULONG_PTR)0x99, 0, smoke_dpa_compare,
                          SMOKE_DPA_LPARAM, DPAS_SORTED );
    out( "found=" );
    out_dec( (ULONG)found );
    out( " missing=0x" );
    out_hex( (ULONG)missing, 8 );
    verdict( found == 3 && missing == DPA_ERR,
             "0x34 is not at index 3, or 0x99 was found" );

    begin( "DPA_EnumCallback(guest callback)" );
    DPA_EnumCallback( hdpa, smoke_dpa_enum, &smoke_dpa_ctx );
    out( "sum=0x" );
    out_hex( smoke_dpa_sum, 4 );
    out( " ref_ok=" );
    out_dec( smoke_dpa_ref_ok );
    verdict( smoke_dpa_sum == SMOKE_DPA_TOTAL && smoke_dpa_ref_ok,
             "the enumerator did not see every element with the caller's pvData" );

    begin( "DPA_DestroyCallback(guest callback)" );
    DPA_DestroyCallback( hdpa, smoke_dpa_destroy, &smoke_dpa_ctx );
    out( "destroyed=" );
    out_dec( smoke_dpa_destroyed );
    verdict( smoke_dpa_destroyed == SMOKE_DPA_N,
             "the destructor did not run once per element" );

    begin( "DSA_DestroyCallback(guest callback)" );
    hdsa = DSA_Create( sizeof(ULONG), 4 );
    for (i = 0; i < 3; i++)
    {
        ULONG v = smoke_dpa_in[i];
        if (DSA_InsertItem( hdsa, DSA_APPEND, &v ) < 0) bad++;
    }
    DSA_DestroyCallback( hdsa, smoke_dsa_destroy, &smoke_dpa_ctx );
    out( "destroyed=" );
    out_dec( smoke_dsa_destroyed );
    out( " sum=0x" );
    out_hex( smoke_dsa_sum, 4 );
    verdict( smoke_dsa_destroyed == 3 && smoke_dsa_sum == SMOKE_DSA_TOTAL,
             "the DSA destructor did not see all three items by value" );
}

/* ==================== comctl32: a callback INSIDE a struct ================
 *
 * A property sheet page carries its dialog procedure in a field, so there is
 * no argument position to name and no RegisterClass to intercept: the port has
 * to copy the struct and swap the field, which is what
 * dlls/comctl32/guestthunk.c does.  This leg proves all three procedures a
 * sheet can carry actually ran, and that the page's own lParam survived every
 * copy on the way:
 *
 *   PSPCB_ADDREF   the page callback, at CreatePropertySheetPage
 *   PSPCB_RELEASE  the page callback, at DestroyPropertySheetPage
 *   PSPCB_CREATE   the page callback, as the page dialog is created
 *   PSCB_PRECREATE the HEADER callback, with the sheet's own template
 *   WM_INITDIALOG  the page's DIALOG PROCEDURE -- the one that is not optional
 *
 * MODELESS, and that is what makes it runnable with nobody at the machine: a
 * modal PropertySheet runs its own message loop until somebody presses a
 * button.  PSH_MODELESS returns the sheet's HWND as soon as the dialog and its
 * first page exist, which is after every callback above has fired.
 */

/* The page's dialog template, in memory (PSP_DLGINDIRECT) because this probe
 * has no resources -- the guest build is linked with no .rc at all.  Written
 * as DWORDs so that both builds lay the same twenty-four bytes down and
 * neither has to trust a packing pragma:
 *
 *   0  style              8  cdit(w) | x(w)      16  cy(w) | menu(w)
 *   4  dwExtendedStyle   12  y(w)    | cx(w)     20  class(w) | title(w)
 *
 * cdit = 0 (no controls), and menu/class/title are all the empty ordinal 0,
 * which is what comctl32's own get_template_size() walks.  No DS_SETFONT, so
 * there is no font block after the title.  comctl32 copies this before it
 * rewrites the style bits, so const is safe. */
static const DWORD smoke_psp_template[6] =
{
    WS_CHILD,
    0,
    0u   | (0u   << 16),
    0u   | (100u << 16),
    60u  | (0u   << 16),
    0u   | (0u   << 16),
};
C_ASSERT( sizeof(DLGTEMPLATE) == 18 );
C_ASSERT( sizeof(smoke_psp_template) == 24 );

#define SMOKE_PSP_COOKIE  ((LPARAM)0x0FACADE5)

static ULONG smoke_psp_cb_mask;      /* 1<<PSPCB_* */
static ULONG smoke_psh_cb_mask;      /* 1<<PSCB_*  */
static ULONG smoke_psp_dlg_init;
static ULONG smoke_psp_cookie_ok = 1;

static UINT CALLBACK smoke_psp_callback( HWND hwnd, UINT msg, PROPSHEETPAGEA *psp )
{
    if (msg < 8) smoke_psp_cb_mask |= 1u << msg;
    /* The page comctl32 hands back is ITS copy of a copy of ours.  lParam is
     * the application's own datum and must have survived both. */
    if (!psp || psp->lParam != SMOKE_PSP_COOKIE) smoke_psp_cookie_ok = 0;
    return 1;
}

static INT CALLBACK smoke_psh_callback( HWND hwnd, UINT msg, LPARAM lp )
{
    if (msg < 8) smoke_psh_cb_mask |= 1u << msg;
    return 0;
}

static INT_PTR CALLBACK smoke_psp_dlgproc( HWND hdlg, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_INITDIALOG)
    {
        const PROPSHEETPAGEA *psp = (const PROPSHEETPAGEA *)lp;

        smoke_psp_dlg_init = 1;
        if (!psp || psp->lParam != SMOKE_PSP_COOKIE) smoke_psp_cookie_ok = 0;
    }
    return FALSE;
}

static void run_propsheet( void )
{
    PROPSHEETHEADERA psh;
    PROPSHEETPAGEA psp;
    HPROPSHEETPAGE hpage;
    INT_PTR ret;
    BYTE *p;
    int i;

    /* memset by hand: no CRT on the guest side. */
    p = (BYTE *)&psp; for (i = 0; i < (int)sizeof(psp); i++) p[i] = 0;
    psp.dwSize      = sizeof(psp);
    psp.dwFlags     = PSP_DLGINDIRECT | PSP_USECALLBACK | PSP_USETITLE;
    psp.hInstance   = GetModuleHandleA( NULL );
    psp.pszTemplate = (const char *)smoke_psp_template;
    psp.pszTitle    = "smoke";
    psp.pfnDlgProc  = smoke_psp_dlgproc;
    psp.lParam      = SMOKE_PSP_COOKIE;
    psp.pfnCallback = smoke_psp_callback;

    /* PSPCB_ADDREF needs a page bigger than V1, which is why dwSize above is
     * the full struct: comctl32 skips the ADDREF for a V1-sized page. */
    begin( "CreatePropertySheetPageA(PSP_USECALLBACK)" );
    hpage = CreatePropertySheetPageA( &psp );
    out( hpage ? "created" : "NULL" );
    out( " cb=0x" );
    out_hex( smoke_psp_cb_mask, 2 );
    verdict( hpage != NULL && (smoke_psp_cb_mask & (1u << PSPCB_ADDREF)),
             "the page callback never saw PSPCB_ADDREF" );

    begin( "DestroyPropertySheetPage" );
    out( DestroyPropertySheetPage( hpage ) ? "TRUE" : "FALSE" );
    out( " cb=0x" );
    out_hex( smoke_psp_cb_mask, 2 );
    verdict( (smoke_psp_cb_mask & (1u << PSPCB_RELEASE)) != 0,
             "the page callback never saw PSPCB_RELEASE" );

    smoke_psp_cb_mask = 0;

    p = (BYTE *)&psh; for (i = 0; i < (int)sizeof(psh); i++) p[i] = 0;
    psh.dwSize      = sizeof(psh);
    psh.dwFlags     = PSH_PROPSHEETPAGE | PSH_MODELESS | PSH_USECALLBACK;
    psh.hInstance   = GetModuleHandleA( NULL );
    psh.pszCaption  = "shell_smoke";
    psh.nPages      = 1;
    psh.nStartPage  = 0;
    psh.ppsp        = &psp;
    psh.pfnCallback = smoke_psh_callback;

    begin( "PropertySheetA(PSH_MODELESS|PSH_PROPSHEETPAGE)" );
    ret = PropertySheetA( &psh );
    if (ret > 0) DestroyWindow( (HWND)(ULONG_PTR)ret );
    out( ret > 0 ? "created" : "failed" );
    out( " psp_cb=0x" );
    out_hex( smoke_psp_cb_mask, 2 );
    out( " psh_cb=0x" );
    out_hex( smoke_psh_cb_mask, 2 );
    out( " dlg_init=" );
    out_dec( smoke_psp_dlg_init );
    out( " cookie_ok=" );
    out_dec( smoke_psp_cookie_ok );
    /* ADDREF and CREATE are required; RELEASE arrives when the sheet is torn
     * down and is printed rather than required, because whether comctl32 frees
     * a page it created is comctl32's business and not the boundary's.  Any
     * difference between the two runs is still caught: the whole mask is
     * printed and the two outputs are compared byte for byte. */
#if SHELL_SMOKE_BREAK == 5
    verdict( ret > 0 &&
             (smoke_psp_cb_mask & ((1u << PSPCB_ADDREF) | (1u << PSPCB_CREATE)))
                 == (1u << PSPCB_RELEASE),                      /* control */
             "the page callbacks did not arrive" );
#else
    verdict( ret > 0 &&
             (smoke_psp_cb_mask & ((1u << PSPCB_ADDREF) | (1u << PSPCB_CREATE)))
                 == ((1u << PSPCB_ADDREF) | (1u << PSPCB_CREATE)) &&
             (smoke_psh_cb_mask & (1u << PSCB_PRECREATE)) != 0 &&
             smoke_psp_dlg_init && smoke_psp_cookie_ok,
             "the page's dialog procedure or its callbacks did not arrive" );
#endif
}

#ifdef SHELL_SMOKE_HOOK
/* ==================== comdlg32: the hook inside the struct ================
 *
 * Every common dialog carries lpfnHook in its one struct argument, and
 * comdlg32 installs it on the dialog it creates.  This is the shape
 * dlls/comdlg32/guestthunk.c swaps IN PLACE, and the in-place part is what
 * lCustData tests below: comdlg32 hands the hook THE CALLER'S OWN
 * OPENFILENAME as WM_INITDIALOG's lParam, so reading the probe's own cookie
 * back out of it proves the pointer the hook received is the one the probe
 * passed and not a copy somebody made on the way.
 *
 * The dialog is CANCELLED FROM INSIDE THE HOOK.  There is nobody at this
 * machine when the gate runs, the dialog is on an Xvfb of the gate's own, and
 * a file dialog that nobody cancels waits forever.  IDCANCEL is POSTED rather
 * than sent: at WM_INITDIALOG the dialog this hook is a child of has not
 * finished its own initialisation and its modal loop has not started, so the
 * message is delivered when it does.
 */
#define SMOKE_HOOK_CUSTDATA  ((LPARAM)0x5A5A1234)

static ULONG smoke_hook_mask;      /* 1 WM_INITDIALOG, 2 CDN_INITDONE */
static ULONG smoke_hook_cust_ok;
static ULONG smoke_hook_calls;

static UINT_PTR CALLBACK smoke_ofn_hook( HWND hdlg, UINT msg, WPARAM wp, LPARAM lp )
{
    smoke_hook_calls++;
    if (msg == WM_INITDIALOG)
    {
        const OPENFILENAMEA *ofn = (const OPENFILENAMEA *)lp;

        smoke_hook_mask |= 1;
        if (ofn && ofn->lCustData == SMOKE_HOOK_CUSTDATA) smoke_hook_cust_ok = 1;
        PostMessageA( GetParent( hdlg ), WM_COMMAND, IDCANCEL, 0 );
    }
    else if (msg == WM_NOTIFY)
    {
        const OFNOTIFYA *n = (const OFNOTIFYA *)lp;

        if (n && n->hdr.code == CDN_INITDONE)
        {
            smoke_hook_mask |= 2;
            PostMessageA( GetParent( hdlg ), WM_COMMAND, IDCANCEL, 0 );
        }
    }
    return 0;
}

static void run_comdlg32_hook( void )
{
    char file[MAX_PATH];
    OPENFILENAMEA ofn;
    BYTE *p;
    int i;
    DWORD err;
    BOOL ok;

    p = (BYTE *)&ofn; for (i = 0; i < (int)sizeof(ofn); i++) p[i] = 0;
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.Flags       = OFN_ENABLEHOOK | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lCustData   = SMOKE_HOOK_CUSTDATA;
    ofn.lpfnHook    = smoke_ofn_hook;

    begin( "GetOpenFileNameA(OFN_ENABLEHOOK) is cancelled by the hook" );
    ok  = GetOpenFileNameA( &ofn );
    err = CommDlgExtendedError();
    out( ok ? "TRUE" : "FALSE" );
    out( " err=0x" );
    out_hex( err, 8 );
    out( " hook=0x" );
    out_hex( smoke_hook_mask, 2 );
    out( " entered=" );
    out_dec( smoke_hook_calls ? 1 : 0 );
    out( " cust_ok=" );
    out_dec( smoke_hook_cust_ok );
    /* FALSE with error 0 is "the user cancelled", which is exactly what the
     * hook made happen.  A refused call would be FALSE with
     * CDERR_INITIALIZATION and a hook that was never entered at all. */
    verdict( !ok && err == 0 && (smoke_hook_mask & 1) != 0 && smoke_hook_cust_ok,
             "the hook was not entered with the caller's own OPENFILENAME" );
}
#endif

static int shell_smoke_run( void )
{
    HWND hwnd;

    out( "shell_smoke: start\n" );

    begin( "message-only window for the cooperative level" );
    hwnd = smoke_window();
    out( hwnd ? "created" : "NULL" );
    verdict( hwnd != NULL, "no window" );

    run_dinput8( hwnd );
    run_d3dx9();
    run_hid();
    run_comctl32();
    run_comctl32_callbacks();
    run_propsheet();
#ifdef SHELL_SMOKE_HOOK
    run_comdlg32_hook();
#endif

    if (hwnd) DestroyWindow( hwnd );

    out( failures ? "shell_smoke: FAIL " : "shell_smoke: PASS " );
    out_dec( (ULONG)(step - failures) );
    out( "/" );
    out_dec( (ULONG)step );
    out( "\n" );
    return failures ? 1 : 0;
}

#ifdef SHELL_SMOKE_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI shell_smoke_entry( void )
{
    ExitProcess( (UINT)shell_smoke_run() );
}
#else
int main( void )
{
    return shell_smoke_run();
}
#endif
