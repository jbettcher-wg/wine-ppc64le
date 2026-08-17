/*
 * gl_smoke -- the native-vs-guest OpenGL runtime gate.
 *
 * ONE source, built TWICE and run TWICE under the same wine: once as a
 * NATIVE ppc64 PE (winegcc, the machine's own architecture, no emulation
 * anywhere in the process) and once as an x86-64 Windows PE run as a GUEST.
 * The two runs must print BYTE-IDENTICAL stdout.
 *
 * That is a stronger claim than "the guest said PASS", and a different one
 * from check-d3d11-smoke.sh's.  Both legs here go through exactly the same
 * Wine opengl32, the same win32u/winex11 GLX path and the same Mesa driver
 * on the same machine; the ONLY thing that differs between them is whether
 * the caller's instructions are ppc64 or x86-64, and therefore whether every
 * call crossed the guest thunk boundary.  Identical bytes out means the
 * boundary changed nothing: not an argument, not a rounding, not a returned
 * pointer, not a matrix.
 *
 * WHAT IT PROVES, STEP BY STEP, AND WHY EACH STEP IS THERE
 *
 *   1  wglGetProcAddress with no current context answers NULL on both legs.
 *      The port's override asks the native module first, so it must inherit
 *      the native module's "no" as well as its "yes".
 *   2-6  a window, a pixel format chosen and set, a context made current.
 *      Reached through opengl32's OWN wglDescribePixelFormat and
 *      wglGetPixelFormat, two of the six exports Wine declares in no header
 *      and the guest thunk would not have had without
 *      dlls/opengl32/opengl32_guest_wgl.h.
 *   7  glGetString: the returned pointer is HOST memory -- a string inside
 *      the native driver -- and the guest reads it directly and prints it.
 *      Byte-identical output is the proof that a returned host pointer is
 *      simply readable, which this port assumes everywhere and should
 *      therefore demonstrate once.
 *   8  wglGetProcAddress("glGetStringi") -> a GUEST-CALLABLE stub that is
 *      then CALLED, once per extension, and the results checksummed.  This
 *      is the whole point of the module: glGetStringi is in no export table
 *      on any Windows machine.
 *   9  wglGetProcAddress of a name nothing implements answers NULL.  A GL
 *      loader tests for exactly this, and a native pointer here would be a
 *      crash it could not diagnose.
 *  10  glClear to a known colour and glReadPixels back: every one of 4096
 *      texels, checked, with the count checked too.
 *  11  an actual triangle, drawn and read back: an interior texel is the
 *      triangle's colour and a corner texel is still the clear colour.
 *  12  THE FLOATING-POINT ARGUMENT PAST XMM3.  glOrtho takes SIX doubles;
 *      MS-x64 has four argument slots, so `near` and `far` travel on the
 *      STACK, not in XMM4/XMM5 -- which do not exist as argument registers.
 *      The host used to read every FP argument out of an XMM register, so
 *      those two arrived as whatever scratch was in XMM4/XMM5: a WRONG
 *      NUMBER, not a crash.  The six values below are chosen so that every
 *      entry of the resulting projection matrix is exactly representable as
 *      a double, and the check is on the RAW BITS.  m[10] and m[14] are
 *      functions of near and far and of nothing else, so they are the two
 *      that go red if the fifth and sixth arguments are lost.
 *  13  the same defect one type narrower, and through an extension entry
 *      point: glMultiTexCoord4fARB's `q` is a FLOAT at argument position 4,
 *      i.e. on the stack, and the entry point itself only exists because
 *      wglGetProcAddress vended it.  All four values are dyadic fractions,
 *      so GL_CURRENT_TEXTURE_COORDS returns them bit-exactly.
 *  14  THE ARGUMENT PAST THE EIGHTH.  glMap2f takes TEN arguments, four of
 *      them floats, so ELFv2 puts `vorder` and the control-point pointer in
 *      the caller's parameter save area and in no register at all.  The
 *      host's floating-point call path filled eight registers and no save
 *      area, so those two never arrived -- again a wrong number rather than a
 *      crash.  glGetMapfv/glGetMapiv read all ten back.
 *
 * NOT COVERED: presentation.  Nothing here calls SwapBuffers -- the readback
 * is from GL_BACK, so the gate needs no visible surface and never takes a
 * display session.  ppc64le/opengl/check-gl-smoke.sh runs it against an
 * Xvfb, because winex11 needs an X server and the person at this machine is
 * using the real ones.
 *
 * GL_SMOKE_BREAK (falsification; the gate builds each variant and requires it
 * to FAIL, because a gate that cannot go red proves nothing):
 *
 *   =1  skip the glClear entirely -- the readback then sees whatever the
 *       driver left in a fresh buffer, and step 10 goes red.
 *   =2  swap R and B in the CHECK's own expectation (the rendered bytes are
 *       still right; the check is deliberately wrong), so all 4096 mismatch.
 *   =3  check texel (0,0) only.  Coverage is part of step 10's claim, so its
 *       verdict requires checked == 4096 as well as mismatches == 0, and an
 *       incomplete scan fails on that arithmetic rather than by luck.
 *   =4  corrupt the expected projection matrix, so step 12 goes red -- proof
 *       that the floating-point check is a check and not a formality.
 *   =5  corrupt the expected `vorder`, so step 14 goes red -- the same proof
 *       for the argument that travels in the parameter save area.
 *
 * NO C RUNTIME on the guest leg, for the reason d3d11_smoke.c gives: the
 * program formats its own output and writes it with WriteFile, so neither
 * libc's nor ucrt's printf can be the source of a byte difference.  The
 * native leg links ucrtbase (winegcc wants a CRT to start a PE) but calls
 * the same hand-written formatters.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef GL_SMOKE_BREAK
#define GL_SMOKE_BREAK 0
#endif

#include <windows.h>
#include <wine/wgl.h>

/* Two of the six OPENGL32 exports no SDK header has ever declared -- see
 * dlls/opengl32/opengl32_guest_wgl.h, which says the same thing to the thunk
 * generator's signature oracle.  Declared here rather than included from
 * there: that header is the module's private business, and this probe is an
 * ordinary application, which is exactly the position a real one is in. */
INT WINAPI wglDescribePixelFormat( HDC hdc, int index, UINT size,
                                   PIXELFORMATDESCRIPTOR *ppfd );
INT WINAPI wglGetPixelFormat( HDC hdc );

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

/* Observations that are TRUE OF THE PORT rather than of OpenGL, and whose
 * answer therefore legitimately differs between the two legs.  They go to
 * stderr so they can never enter the transcript the two legs are diffed
 * against; check-gl-smoke.sh reads them from the guest run only.
 *
 * Assembled and written in ONE call: stderr is where the port's own debug
 * channels write too, and a line emitted in three WriteFiles can arrive with
 * an err:seh line spliced through the middle of it, which is not something a
 * grep should have to survive. */
static void note_ptr( const char *label, ULONGLONG v )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[128];
    int n = 0, i;

    while (label[n] && n < 100) { buf[n] = label[n]; n++; }
    buf[n++] = ' '; buf[n++] = 'p'; buf[n++] = '=';
    for (i = 15; i >= 0; i--) buf[n++] = hex[(v >> (4 * i)) & 0xf];
    buf[n++] = '\n';
    buf[n] = 0;
    out_fd( GetStdHandle( STD_ERROR_HANDLE ), buf );
}

static void out_hex_to( HANDLE h, ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
    out_fd( h, buf );
}

static void out_hex( ULONGLONG v, int digits )
{
    out_hex_to( GetStdHandle( STD_OUTPUT_HANDLE ), v, digits );
}

static void out_dec( ULONG v )
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v);
    out( buf + i );
}

/* A driver string, printed with anything unprintable escaped, so the
 * transcript is diffable whatever the driver put there. */
static void out_str_bounded( const GLubyte *s, UINT max )
{
    char buf[2];
    UINT i;

    if (!s) { out( "(null)" ); return; }
    buf[1] = 0;
    for (i = 0; i < max && s[i]; i++)
    {
        if (s[i] < 0x20 || s[i] > 0x7e) { out( "?" ); continue; }
        buf[0] = (char)s[i];
        out( buf );
    }
}

/* The bits of a double and of a float, so a floating-point value can be
 * compared and printed EXACTLY.  A decimal rendering would need a formatter
 * this probe deliberately does not have, and would hide the last bit --
 * which is the bit a marshalling bug moves. */
static ULONGLONG dbits( double d )
{
    union { double d; ULONGLONG u; } c;
    c.d = d;
    return c.u;
}

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

static DWORD fnv1a_str( DWORD hash, const char *s )
{
    UINT n = 0;
    while (s[n]) n++;
    return fnv1a( hash, (const BYTE *)s, n );
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

/* The clear colour, and its rounding, spelled out rather than assumed.  For
 * an 8-bit normalised channel GL converts with round(c * 255):
 *
 *   R 0.00f * 255 =   0.00 -> 0x00   exact
 *   G 0.25f * 255 =  63.75 -> 0x40   closer to 64 than to 63 under any
 *                                    nearest-rounding rule
 *   B 0.50f * 255 = 127.50 -> 0x80   an exact tie, and 128 is the answer
 *                                    under both round-half-even and
 *                                    round-half-away-from-zero
 *   A 1.00f * 255 = 255.00 -> 0xff   exact
 */
#define CLEAR_R 0.00f
#define CLEAR_G 0.25f
#define CLEAR_B 0.50f
#define CLEAR_A 1.00f
static const BYTE clear_rgba[4] = { 0x00, 0x40, 0x80, 0xff };

#define FB_W 64
#define FB_H 64

/* glOrtho's six doubles.  Chosen so every matrix entry below is a dyadic
 * fraction and therefore exact:  2/(r-l) = 2/8, 2/(t-b) = 2/16,
 * -2/(f-n) = -2/8, -(r+l)/(r-l) = -4/8, -(t+b)/(t-b) = -8/16,
 * -(f+n)/(f-n) = -10/8. */
#define ORTHO_L (-2.0)
#define ORTHO_R  ( 6.0)
#define ORTHO_B (-4.0)
#define ORTHO_T (12.0)
#define ORTHO_N  ( 1.0)   /* argument 4: on the STACK under MS-x64 */
#define ORTHO_F  ( 9.0)   /* argument 5: on the STACK under MS-x64 */

/* glMultiTexCoord4fARB's four floats; `q` is argument 4, on the stack. */
#define MTC_S 0.25f
#define MTC_T 0.5f
#define MTC_R 0.75f
#define MTC_Q 0.375f

/* glMap2f's domain and orders.  u1/u2 are arguments 1 and 2 (XMM1, XMM2),
 * v1/v2 are arguments 5 and 6 (the guest's stack) and vorder is argument 8
 * (ELFv2's parameter save area).  All four floats are dyadic. */
#define MAP_U1      0.5f
#define MAP_U2      2.5f
#define MAP_V1     (-1.5f)
#define MAP_V2      3.25f
#define MAP_UORDER  2
#define MAP_VORDER  2

typedef const GLubyte * (WINAPI *PFN_getstringi)( GLenum, GLuint );
typedef void (WINAPI *PFN_multitexcoord4f)( GLenum, GLfloat, GLfloat, GLfloat, GLfloat );

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#endif
#ifndef GL_CURRENT_TEXTURE_COORDS
#define GL_CURRENT_TEXTURE_COORDS 0x0B03
#endif

static LRESULT WINAPI smoke_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    return DefWindowProcA( hwnd, msg, wp, lp );
}

static int gl_smoke_run( void )
{
    static BYTE pixels[FB_W * FB_H * 4];
    WNDCLASSA wc;
    HWND hwnd = NULL;
    HDC hdc = NULL;
    HGLRC ctx = NULL;
    PIXELFORMATDESCRIPTOR pfd, got;
    int fmt = 0, described = 0;
    PROC p;
    PFN_getstringi p_getstringi = NULL;
    PFN_multitexcoord4f p_multitexcoord4f = NULL;
    UINT i;

    out( "gl_smoke: start\n" );

    /* ---- 1: wglGetProcAddress with no current context ------------------- */
    begin( "wglGetProcAddress(glCreateShader) with no current context" );
    p = wglGetProcAddress( "glCreateShader" );
    out( "p=" );
    out( p ? "non-null" : "NULL" );
    verdict( p == NULL, "an address was vended with no context to vend it for" );

    /* ---- 2: a window and its DC ----------------------------------------- */
    begin( "window + DC" );
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = smoke_wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandleA( NULL );
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "gl_smoke_class";
    RegisterClassA( &wc );
    hwnd = CreateWindowExA( 0, "gl_smoke_class", "gl_smoke", WS_POPUP,
                            0, 0, FB_W, FB_H, NULL, NULL, wc.hInstance, NULL );
    if (hwnd)
    {
        ShowWindow( hwnd, SW_SHOW );
        hdc = GetDC( hwnd );
    }
    out( "hwnd=" ); out( hwnd ? "yes" : "no" );
    out( " hdc=" );  out( hdc ? "yes" : "no" );
    verdict( hwnd && hdc, "no window or no DC" );
    if (!hdc) goto done;

    /* ---- 3: ChoosePixelFormat -------------------------------------------- */
    begin( "ChoosePixelFormat(double-buffered RGBA, 24-bit depth)" );
    for (i = 0; i < sizeof(pfd); i++) ((BYTE *)&pfd)[i] = 0;
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    fmt = ChoosePixelFormat( hdc, &pfd );
    out( "fmt>0=" ); out( fmt > 0 ? "yes" : "no" );
    verdict( fmt > 0, "no pixel format matched" );
    if (fmt <= 0) goto done;

    /* ---- 4: opengl32's own wglDescribePixelFormat ------------------------ */
    /* One of the six exports Wine declares in no header; without a hand-written
     * declaration for the oracle the guest module would not have it at all. */
    begin( "wglDescribePixelFormat" );
    for (i = 0; i < sizeof(got); i++) ((BYTE *)&got)[i] = 0;
    described = wglDescribePixelFormat( hdc, fmt, sizeof(got), &got );
    out( "n>0=" ); out( described > 0 ? "yes" : "no" );
    out( " nSize=" ); out_dec( got.nSize );
    out( " rgba=" ); out( got.iPixelType == PFD_TYPE_RGBA ? "yes" : "no" );
    out( " colorbits>=24=" ); out( got.cColorBits >= 24 ? "yes" : "no" );
    out( " doublebuffer=" ); out( (got.dwFlags & PFD_DOUBLEBUFFER) ? "yes" : "no" );
    verdict( described > 0 && got.nSize == sizeof(got) && got.iPixelType == PFD_TYPE_RGBA
             && got.cColorBits >= 24 && (got.dwFlags & PFD_DOUBLEBUFFER),
             "the format does not describe itself the way it was chosen" );

    /* ---- 5: SetPixelFormat, read back through wglGetPixelFormat ---------- */
    begin( "SetPixelFormat + wglGetPixelFormat" );
    {
        BOOL set = SetPixelFormat( hdc, fmt, &pfd );
        int back = wglGetPixelFormat( hdc );
        out( "set=" ); out( set ? "yes" : "no" );
        out( " matches=" ); out( back == fmt ? "yes" : "no" );
        verdict( set && back == fmt, "the DC does not report the format that was set" );
        if (!set) goto done;
    }

    /* ---- 6: a context, made current -------------------------------------- */
    begin( "wglCreateContext + wglMakeCurrent" );
    ctx = wglCreateContext( hdc );
    out( "ctx=" ); out( ctx ? "yes" : "no" );
    if (ctx)
    {
        BOOL cur = wglMakeCurrent( hdc, ctx );
        out( " current=" ); out( cur ? "yes" : "no" );
        out( " getcurrentcontext=" ); out( wglGetCurrentContext() == ctx ? "match" : "differs" );
        out( " getcurrentdc=" ); out( wglGetCurrentDC() == hdc ? "match" : "differs" );
        verdict( cur && wglGetCurrentContext() == ctx && wglGetCurrentDC() == hdc,
                 "the context did not become current" );
        if (!cur) goto done;
    }
    else
    {
        verdict( FALSE, "no context" );
        goto done;
    }

    /* ---- 7: glGetString -- a HOST pointer, read by the guest -------------- */
    begin( "glGetString" );
    out( "vendor=\"" );   out_str_bounded( glGetString( GL_VENDOR ), 64 );
    out( "\" renderer=\"" ); out_str_bounded( glGetString( GL_RENDERER ), 64 );
    out( "\" version=\"" );  out_str_bounded( glGetString( GL_VERSION ), 64 );
    out( "\"" );
    verdict( glGetString( GL_VENDOR ) && glGetString( GL_RENDERER )
             && glGetString( GL_VERSION ) && glGetError() == GL_NO_ERROR,
             "a driver string is missing or GL is already in error" );

    /* ---- 8: an entry point that exists in NO export table ---------------- */
    begin( "wglGetProcAddress(glGetStringi), then call it" );
    p_getstringi = (PFN_getstringi)wglGetProcAddress( "glGetStringi" );
    out( "p=" ); out( p_getstringi ? "non-null" : "NULL" );
    if (p_getstringi)
    {
        GLint next = 0;
        DWORD hash = 0x811c9dc5u;
        UINT counted = 0;

        glGetIntegerv( GL_NUM_EXTENSIONS, &next );
        for (i = 0; i < (UINT)next && i < 16; i++)
        {
            const GLubyte *s = p_getstringi( GL_EXTENSIONS, i );
            if (!s) break;
            hash = fnv1a_str( hash, (const char *)s );
            counted++;
        }
        out( " numext>0=" ); out( next > 0 ? "yes" : "no" );
        out( " counted=" ); out_dec( counted );
        out( " fnv=0x" ); out_hex( hash, 8 );
        verdict( next > 0 && counted == (next < 16 ? (UINT)next : 16)
                 && hash != 0x811c9dc5u,
                 "glGetStringi did not enumerate the extensions" );
    }
    else verdict( FALSE, "glGetStringi was not vended" );

    /* ---- 9: a name nothing implements ------------------------------------ */
    begin( "wglGetProcAddress(glNoSuchEntryPointWINE)" );
    p = wglGetProcAddress( "glNoSuchEntryPointWINE" );
    out( "p=" ); out( p ? "non-null" : "NULL" );
    verdict( p == NULL, "an address was vended for a name nothing implements" );

    /* The port-specific observations, on stderr; see note_ptr() above. */
    note_ptr( "gl_smoke_note: glDebugMessageCallback",
              (ULONGLONG)(ULONG_PTR)wglGetProcAddress( "glDebugMessageCallback" ) );

    /* ---- 10: clear to a known colour and read it back --------------------- */
    begin( "glClear + glReadPixels" );
    {
        UINT mismatches = 0, checked = 0;
        BYTE want[4];

        want[0] = clear_rgba[0]; want[1] = clear_rgba[1];
        want[2] = clear_rgba[2]; want[3] = clear_rgba[3];
#if GL_SMOKE_BREAK == 2
        { BYTE t = want[0]; want[0] = want[2]; want[2] = t; }
#endif
        glViewport( 0, 0, FB_W, FB_H );
        glDrawBuffer( GL_BACK );
        glClearColor( CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A );
#if GL_SMOKE_BREAK != 1
        glClear( GL_COLOR_BUFFER_BIT );
#endif
        glFinish();
        glReadBuffer( GL_BACK );
        glPixelStorei( GL_PACK_ALIGNMENT, 1 );
        glReadPixels( 0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
#if GL_SMOKE_BREAK == 3
        for (i = 0; i < 1; i++)
#else
        for (i = 0; i < FB_W * FB_H; i++)
#endif
        {
            checked++;
            if (pixels[i * 4 + 0] != want[0] || pixels[i * 4 + 1] != want[1] ||
                pixels[i * 4 + 2] != want[2] || pixels[i * 4 + 3] != want[3])
                mismatches++;
        }
        out( "checked=" ); out_dec( checked );
        out( " mismatches=" ); out_dec( mismatches );
        out( " texel0=" );
        out_hex( pixels[0], 2 ); out_hex( pixels[1], 2 );
        out_hex( pixels[2], 2 ); out_hex( pixels[3], 2 );
        verdict( checked == FB_W * FB_H && mismatches == 0,
                 "the framebuffer is not the colour it was cleared to, or not all "
                 "of it was looked at" );
    }

    /* ---- 11: an actual triangle ------------------------------------------ */
    begin( "triangle + glReadPixels" );
    {
        const UINT centre = (FB_H / 2) * FB_W + (FB_W / 2);
        BOOL centre_ok, corner_ok;

        glMatrixMode( GL_PROJECTION );
        glLoadIdentity();
        glMatrixMode( GL_MODELVIEW );
        glLoadIdentity();
        glClear( GL_COLOR_BUFFER_BIT );
        glBegin( GL_TRIANGLES );
        glColor3f( 1.0f, 0.0f, 0.0f );
        glVertex2f( -0.9f, -0.9f );
        glVertex2f(  0.9f, -0.9f );
        glVertex2f(  0.0f,  0.9f );
        glEnd();
        glFinish();
        glReadPixels( 0, 0, FB_W, FB_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
        centre_ok = pixels[centre * 4 + 0] == 0xff && pixels[centre * 4 + 1] == 0x00 &&
                    pixels[centre * 4 + 2] == 0x00;
        corner_ok = pixels[0] == clear_rgba[0] && pixels[1] == clear_rgba[1] &&
                    pixels[2] == clear_rgba[2];
        out( "centre=" );
        out_hex( pixels[centre * 4 + 0], 2 ); out_hex( pixels[centre * 4 + 1], 2 );
        out_hex( pixels[centre * 4 + 2], 2 );
        out( " corner=" );
        out_hex( pixels[0], 2 ); out_hex( pixels[1], 2 ); out_hex( pixels[2], 2 );
        verdict( centre_ok && corner_ok,
                 "the triangle did not cover the centre, or covered the corner" );
    }

    /* ---- 12: glOrtho -- two of its six doubles travel on the stack -------- */
    begin( "glOrtho(6 doubles) -> GL_PROJECTION_MATRIX" );
    {
        double m[16];
        double want0  = 2.0 / (ORTHO_R - ORTHO_L);
        double want5  = 2.0 / (ORTHO_T - ORTHO_B);
        double want10 = -2.0 / (ORTHO_F - ORTHO_N);
        double want12 = -(ORTHO_R + ORTHO_L) / (ORTHO_R - ORTHO_L);
        double want13 = -(ORTHO_T + ORTHO_B) / (ORTHO_T - ORTHO_B);
        double want14 = -(ORTHO_F + ORTHO_N) / (ORTHO_F - ORTHO_N);

#if GL_SMOKE_BREAK == 4
        want14 += 1.0;
#endif
        for (i = 0; i < 16; i++) m[i] = 0.0;
        glMatrixMode( GL_PROJECTION );
        glLoadIdentity();
        glOrtho( ORTHO_L, ORTHO_R, ORTHO_B, ORTHO_T, ORTHO_N, ORTHO_F );
        glGetDoublev( GL_PROJECTION_MATRIX, m );
        out( "m0=" );  out_hex( dbits( m[0] ), 16 );
        out( " m5=" ); out_hex( dbits( m[5] ), 16 );
        out( " m10=" ); out_hex( dbits( m[10] ), 16 );
        out( " m12=" ); out_hex( dbits( m[12] ), 16 );
        out( " m13=" ); out_hex( dbits( m[13] ), 16 );
        out( " m14=" ); out_hex( dbits( m[14] ), 16 );
        verdict( dbits( m[0] ) == dbits( want0 ) && dbits( m[5] ) == dbits( want5 ) &&
                 dbits( m[10] ) == dbits( want10 ) && dbits( m[12] ) == dbits( want12 ) &&
                 dbits( m[13] ) == dbits( want13 ) && dbits( m[14] ) == dbits( want14 ),
                 "the projection matrix is not the one those six doubles define -- "
                 "the fifth and sixth arguments are the ones that travel on the stack" );
        glLoadIdentity();
    }

    /* ---- 13: a float at argument position 4, through an extension --------- */
    begin( "wglGetProcAddress(glMultiTexCoord4fARB), call it, read it back" );
    p_multitexcoord4f = (PFN_multitexcoord4f)wglGetProcAddress( "glMultiTexCoord4fARB" );
    out( "p=" ); out( p_multitexcoord4f ? "non-null" : "NULL" );
    if (p_multitexcoord4f)
    {
        float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        p_multitexcoord4f( GL_TEXTURE0_ARB, MTC_S, MTC_T, MTC_R, MTC_Q );
        glGetFloatv( GL_CURRENT_TEXTURE_COORDS, f );
        out( " s=" ); out_hex( fbits( f[0] ), 8 );
        out( " t=" ); out_hex( fbits( f[1] ), 8 );
        out( " r=" ); out_hex( fbits( f[2] ), 8 );
        out( " q=" ); out_hex( fbits( f[3] ), 8 );
        verdict( fbits( f[0] ) == fbits( MTC_S ) && fbits( f[1] ) == fbits( MTC_T ) &&
                 fbits( f[2] ) == fbits( MTC_R ) && fbits( f[3] ) == fbits( MTC_Q ),
                 "the current texture coordinates are not the four floats passed -- "
                 "q is the one that travels on the stack" );
    }
    else verdict( FALSE, "glMultiTexCoord4fARB was not vended" );

    /* ---- 14: an argument PAST THE EIGHTH, on a call that also has floats --- */
    /* glMap2f is GL 1.1 and takes TEN arguments, four of them GLfloat:
     *
     *   0 target  1 u1  2 u2  3 ustride  4 uorder
     *   5 v1  6 v2  7 vstride  8 vorder  9 points
     *
     * ELFv2 gives arguments 0-7 the registers r3-r10 and puts everything from
     * the ninth onward in the caller's parameter save area -- and the host's
     * floating-point call path used to fill eight registers and no save area
     * at all, so `vorder` and the control-point pointer simply never arrived.
     * The integer-only path never had the bug: it is an ordinary C call and
     * the compiler lays the frame out.  This one is hand-written assembly, so
     * the frame had to be written too.
     *
     * The readback covers both defects at once and by three different routes:
     * GL_DOMAIN returns u1 and u2 (XMM1, XMM2) beside v1 and v2 (arguments 5
     * and 6, on the guest's stack); GL_ORDER returns uorder (a register)
     * beside vorder (argument 8, in the save area); and GL_COEFF can only
     * answer at all if the control-point pointer -- argument 9 -- arrived. */
    begin( "glMap2f(10 args, 4 floats, 2 past the eighth) -> glGetMapfv/iv" );
    {
        static const GLfloat pts[2 * 2 * 3] = {
            0.00f, 0.25f, 0.50f,   0.75f, 1.00f, 1.25f,
            1.50f, 1.75f, 2.00f,   2.25f, 2.50f, 2.75f,
        };
        GLfloat dom[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        GLfloat coeff[2 * 2 * 3];
        GLint ord[2] = { 0, 0 };
        DWORD hash = 0x811c9dc5u;
        float sum = 0.0f;
        GLint want_vorder = MAP_VORDER;

#if GL_SMOKE_BREAK == 5
        want_vorder++;
#endif
        for (i = 0; i < 2 * 2 * 3; i++) coeff[i] = 0.0f;
        glMap2f( GL_MAP2_VERTEX_3, MAP_U1, MAP_U2, 6, MAP_UORDER,
                 MAP_V1, MAP_V2, 3, MAP_VORDER, pts );
        glGetMapfv( GL_MAP2_VERTEX_3, GL_DOMAIN, dom );
        glGetMapiv( GL_MAP2_VERTEX_3, GL_ORDER, ord );
        glGetMapfv( GL_MAP2_VERTEX_3, GL_COEFF, coeff );
        for (i = 0; i < 2 * 2 * 3; i++)
        {
            hash = fnv1a( hash, (const BYTE *)&coeff[i], sizeof(coeff[i]) );
            sum += coeff[i];
        }
        out( "u1=" ); out_hex( fbits( dom[0] ), 8 );
        out( " u2=" ); out_hex( fbits( dom[1] ), 8 );
        out( " v1=" ); out_hex( fbits( dom[2] ), 8 );
        out( " v2=" ); out_hex( fbits( dom[3] ), 8 );
        out( " uorder=" ); out_dec( (ULONG)ord[0] );
        out( " vorder=" ); out_dec( (ULONG)ord[1] );
        out( " coeffsum=" ); out_hex( fbits( sum ), 8 );
        out( " coefffnv=0x" ); out_hex( hash, 8 );
        /* Every control point is a multiple of 0.25 and they total 16.5, so
         * the sum is exact whatever order the driver stores them in -- which
         * is what makes this a check on "did argument 9 arrive" rather than a
         * check on Mesa's coefficient layout. */
        verdict( fbits( dom[0] ) == fbits( MAP_U1 ) && fbits( dom[1] ) == fbits( MAP_U2 ) &&
                 fbits( dom[2] ) == fbits( MAP_V1 ) && fbits( dom[3] ) == fbits( MAP_V2 ) &&
                 ord[0] == MAP_UORDER && ord[1] == want_vorder &&
                 fbits( sum ) == fbits( 16.5f ),
                 "the evaluator does not hold the ten arguments it was given -- "
                 "vorder and the control points are the two that travel in the "
                 "parameter save area rather than a register" );
    }

    /* ---- 15: teardown ----------------------------------------------------- */
    begin( "teardown" );
    {
        BOOL uncur = wglMakeCurrent( NULL, NULL );
        BOOL del = wglDeleteContext( ctx );
        ctx = NULL;
        out( "uncurrent=" ); out( uncur ? "yes" : "no" );
        out( " deleted=" ); out( del ? "yes" : "no" );
        verdict( uncur && del, "the context did not come apart cleanly" );
    }

done:
    if (ctx) { wglMakeCurrent( NULL, NULL ); wglDeleteContext( ctx ); }
    if (hdc && hwnd) ReleaseDC( hwnd, hdc );
    if (hwnd) DestroyWindow( hwnd );

    out( failures ? "gl_smoke: FAIL " : "gl_smoke: PASS " );
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

#if defined(GL_SMOKE_NATIVE)
/* The native ppc64 PE leg: winegcc links a CRT, so this is an ordinary main. */
int main( void )
{
    return gl_smoke_run();
}
#else
/* The guest leg has no C runtime: this IS the image entry point. */
void WINAPI gl_smoke_entry( void )
{
    ExitProcess( (UINT)gl_smoke_run() );
}
#endif
