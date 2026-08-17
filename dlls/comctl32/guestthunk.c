/*
 * comctl32.dll -- the callbacks an x86-64 guest hands to native comctl32.
 *
 * comctl32 takes guest code in two shapes, and neither is reachable from
 * dlls/ntdll/signal_ppc64.c's override table:
 *
 *   * AS A PLAIN ARGUMENT.  DPA_Sort, DPA_Search, DPA_EnumCallback,
 *     DPA_DestroyCallback and DSA_DestroyCallback each take a function pointer
 *     that native comctl32 calls once per element from a native frame.  That
 *     shape IS what the override table describes -- but a row there is a row
 *     in a file this module does not own, and it does not need one: ntdll now
 *     exports the trampoline factory itself, so the module that knows what its
 *     own argument means does the wrapping.
 *
 *   * INSIDE A STRUCT.  A PROPSHEETPAGE carries `pfnDlgProc`, and it is not
 *     optional the way a comdlg32 hook is: it is the page's dialog procedure,
 *     comctl32 stores it and dispatches every message the page receives to it.
 *     PROPSHEETPAGE additionally carries an optional `pfnCallback`
 *     (PSP_USECALLBACK) and PROPSHEETHEADER carries `pfnCallback`
 *     (PSH_USECALLBACK).  None of the three ever passes through RegisterClass
 *     or SetWindowLongPtr(GWLP_WNDPROC), so the rows that make a guest's own
 *     window work do nothing here.  No argument-position mask can name a field
 *     inside a struct; this is the emu_RegisterClass shape, written in the
 *     module that owns the struct.
 *
 * A pass-through would hand native comctl32 an x86-64 dialog procedure, and
 * user32's callback dispatcher SWALLOWS what a dialog procedure raises -- the
 * exact silent failure the emu_RegisterClass banner records for DOOM's
 * WNDPROC: "ignoring exception c000001d", a page that never processes a
 * message, and no error anywhere.
 *
 * WHAT IS STILL REFUSED, AND WHY IT IS A DIFFERENT REASON THAN IT WAS.
 * SetWindowSubclass and RemoveWindowSubclass are still absent from the guest
 * export list, and no longer because nothing can wrap a callback: because a
 * SUBCLASSPROC takes SIX arguments --
 *
 *     LRESULT CALLBACK (HWND, UINT, WPARAM, LPARAM, UINT_PTR uIdSubclass,
 *                       DWORD_PTR dwRefData)
 *
 * -- and ntdll's trampoline carries FOUR.  call_guest_function_args() in
 * dlls/ntdll/signal_ppc64.c says so in as many words ("A callback with stack
 * arguments (five or more) would need a thunk that builds a frame; nothing in
 * the corpus has one, and this is where it would go"): its guest thunk loads
 * RCX/RDX/R8/R9 and tail-jumps, so arguments five and six -- which MS-x64
 * passes on the stack -- are whatever happened to be there.  A guest
 * SUBCLASSPROC would receive a correct hwnd/msg/wParam/lParam and GARBAGE for
 * its subclass id and its own reference data: a silent wrong answer, and
 * strictly worse than a missing import that kills the process at the
 * registration.  Nothing in this file can fix it -- the frame has to be built
 * by the side that owns the guest run-entry primitive, and ntdll exports no
 * other way in.  comctl32.thunks carries the exact handoff.
 *
 * IDENTITY, since it is what a Set/Remove pair turns on and it needs no
 * machinery here: the trampoline pool is keyed on (guest target, return width)
 * and returns the SAME stub for the same pair, for the life of the process.
 * So the day the six-argument thunk lands, wrapping in both SetWindowSubclass
 * and RemoveWindowSubclass is all the identity those two need -- there is no
 * mapping table to keep, and a separate one would be a second source of truth
 * for something the pool already guarantees.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "commctrl.h"
#include "prsht.h"

#include "comctl32.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(guestcb);

/* ------------------------------------------------- ntdll's callback factory
 *
 * Resolved by name at first use rather than linked against -- the
 * resolve_gl_entry_point discipline in dlls/opengl32/wgl.c, and the same one
 * dlls/dinput8/guestcom.c and dlls/comdlg32/guestthunk.c keep.  A tree whose
 * ntdll predates the export refuses HERE, loudly and by name, rather than
 * failing to load comctl32 and taking every control down with it. */
static void *(CDECL *guest_wrap_callback)( void *fn, BOOL wide );
static LONG wrap_resolved;

static BOOL resolve_wrap_callback(void)
{
    UNICODE_STRING ntdllW;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    if (InterlockedCompareExchange( &wrap_resolved, 1, 0 ))
        return guest_wrap_callback != NULL;

    RtlInitUnicodeString( &ntdllW, L"ntdll.dll" );
    RtlInitAnsiString( &name, "__wine_guest_wrap_callback" );
    if (LdrGetDllHandle( NULL, 0, &ntdllW, &ntdll ) ||
        LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        ERR( "comctl32: this ntdll exports no __wine_guest_wrap_callback; a "
             "guest comparator or dialog procedure cannot be swapped for a "
             "trampoline, and these entry points will refuse rather than let "
             "native comctl32 call x86-64 bytes\n" );
        return FALSE;
    }
    guest_wrap_callback = proc;
    return TRUE;
}

/* -> the trampoline, or NULL if there is none to be had.  NULL in is NULL out
 * and not a failure: a caller passing no callback at all is comctl32's
 * business to report, not this file's. */
static void *guest_wrap( void *fn, BOOL wide, const char *what )
{
    void *wrapped;

    if (!fn) return NULL;
    if (!resolve_wrap_callback()) return NULL;
    if (!(wrapped = guest_wrap_callback( fn, wide )))
    {
        ERR( "comctl32: the trampoline pool would not mint a stub for %s %p\n",
             what, fn );
        return NULL;
    }
    TRACE( "%s %p -> trampoline %p\n", what, fn, wrapped );
    return wrapped;
}

/* TRUE when a callback the caller really did supply could not be served, which
 * is the one condition every wrapper below refuses on.  Written once so that
 * no refusal can be spelled subtly differently from the others. */
static BOOL guest_wrap_failed( const void *in, const void *out,
                               const char *export_name )
{
    if (!in || out) return FALSE;
    ERR( "comctl32: refusing %s for an x86-64 guest -- its callback %p could "
         "not be swapped for a native trampoline, and native comctl32 would "
         "call it directly\n", export_name, in );
    return TRUE;
}

/* ------------------------------------------------------- the DPA/DSA family
 *
 * All five take one bare function pointer that native comctl32 calls once per
 * element.  Every one of those callbacks returns INT, so all five take the
 * NARROW trampoline: an ELFv2 caller is entitled to a sign-extended 32-bit
 * result, and a comparator returning -1 must not arrive as 0xffffffff.  All
 * fit inside the four arguments the trampoline carries --
 *
 *     PFNDPACOMPARE       INT (LPVOID, LPVOID, LPARAM)   3
 *     PFNDPAENUMCALLBACK  INT (LPVOID, LPVOID)           2
 *     PFNDSAENUMCALLBACK  INT (LPVOID, LPVOID)           2
 *
 * -- which is what makes these five servable and SetWindowSubclass not.
 */

BOOL WINAPI __wine_guest_DPA_Sort( HDPA hdpa, PFNDPACOMPARE cmp, LPARAM lParam )
{
    void *wrapped = guest_wrap( (void *)cmp, FALSE, "DPA_Sort comparator" );

    if (guest_wrap_failed( (const void *)cmp, wrapped, "DPA_Sort" )) return FALSE;
    return DPA_Sort( hdpa, (PFNDPACOMPARE)wrapped, lParam );
}

INT WINAPI __wine_guest_DPA_Search( HDPA hdpa, void *search, INT start,
                                    PFNDPACOMPARE cmp, LPARAM lParam, UINT options )
{
    void *wrapped = guest_wrap( (void *)cmp, FALSE, "DPA_Search comparator" );

    if (guest_wrap_failed( (const void *)cmp, wrapped, "DPA_Search" )) return DPA_ERR;
    return DPA_Search( hdpa, search, start, (PFNDPACOMPARE)wrapped, lParam, options );
}

void WINAPI __wine_guest_DPA_EnumCallback( HDPA hdpa, PFNDPAENUMCALLBACK enumProc,
                                           void *lParam )
{
    void *wrapped = guest_wrap( (void *)enumProc, FALSE,
                                "DPA_EnumCallback callback" );

    if (guest_wrap_failed( (const void *)enumProc, wrapped, "DPA_EnumCallback" ))
        return;
    DPA_EnumCallback( hdpa, (PFNDPAENUMCALLBACK)wrapped, lParam );
}

/* The two destroying forms return void and destroy the container as well as
 * enumerating it.  A refusal therefore LEAKS the container, deliberately: the
 * alternative is to destroy it while silently skipping the application's own
 * per-element destructor, which is a wrong answer nobody can see.  A leak with
 * an ERR beside it is the honest failure. */
void WINAPI __wine_guest_DPA_DestroyCallback( HDPA hdpa, PFNDPAENUMCALLBACK enumProc,
                                              void *lParam )
{
    void *wrapped = guest_wrap( (void *)enumProc, FALSE,
                                "DPA_DestroyCallback callback" );

    if (guest_wrap_failed( (const void *)enumProc, wrapped, "DPA_DestroyCallback" ))
        return;
    DPA_DestroyCallback( hdpa, (PFNDPAENUMCALLBACK)wrapped, lParam );
}

void WINAPI __wine_guest_DSA_DestroyCallback( HDSA hdsa, PFNDSAENUMCALLBACK enumProc,
                                              void *lParam )
{
    void *wrapped = guest_wrap( (void *)enumProc, FALSE,
                                "DSA_DestroyCallback callback" );

    if (guest_wrap_failed( (const void *)enumProc, wrapped, "DSA_DestroyCallback" ))
        return;
    DSA_DestroyCallback( hdsa, (PFNDSAENUMCALLBACK)wrapped, lParam );
}

/* ------------------------------------------------------- the property sheets
 *
 * PROPSHEETPAGEA and PROPSHEETPAGEW differ only in the type of four string
 * pointers, and PROPSHEETHEADERA/W likewise, so one set of offsets serves both
 * and the A form is used to reach the fields this file swaps.  PINNED, because
 * an offset that was wrong here would rewrite eight bytes of somebody else's
 * field in a struct that is then handed to native comctl32. */
C_ASSERT( sizeof(PROPSHEETPAGEA) == sizeof(PROPSHEETPAGEW) );
C_ASSERT( FIELD_OFFSET(PROPSHEETPAGEA, dwSize) == 0 );
C_ASSERT( FIELD_OFFSET(PROPSHEETPAGEA, dwFlags) ==
          FIELD_OFFSET(PROPSHEETPAGEW, dwFlags) );
C_ASSERT( FIELD_OFFSET(PROPSHEETPAGEA, pfnDlgProc) ==
          FIELD_OFFSET(PROPSHEETPAGEW, pfnDlgProc) );
C_ASSERT( FIELD_OFFSET(PROPSHEETPAGEA, pfnCallback) ==
          FIELD_OFFSET(PROPSHEETPAGEW, pfnCallback) );
C_ASSERT( FIELD_OFFSET(PROPSHEETPAGEA, pfnCallback) < PROPSHEETPAGEA_V1_SIZE );

C_ASSERT( sizeof(PROPSHEETHEADERA) == sizeof(PROPSHEETHEADERW) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, dwSize) == 0 );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, dwFlags) ==
          FIELD_OFFSET(PROPSHEETHEADERW, dwFlags) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, nPages) ==
          FIELD_OFFSET(PROPSHEETHEADERW, nPages) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, ppsp) ==
          FIELD_OFFSET(PROPSHEETHEADERW, ppsp) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, phpage) ==
          FIELD_OFFSET(PROPSHEETHEADERW, phpage) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, pfnCallback) ==
          FIELD_OFFSET(PROPSHEETHEADERW, pfnCallback) );
C_ASSERT( FIELD_OFFSET(PROPSHEETHEADERA, pfnCallback) < PROPSHEETHEADERA_V1_SIZE );

/* Sanity ceilings, and neither is a policy: a page array is walked by reading
 * a size out of guest memory and stepping by it, so a corrupt dwSize must stop
 * this file rather than have it copy an arbitrary amount.  Real sheets have a
 * handful of pages of about a hundred bytes each. */
#define GUEST_PSP_MAX_PAGES  256
#define GUEST_PSP_MAX_BYTES  (256 * 1024)

/* Swap the two guest procedures in ONE already-copied page, in place.
 *
 * pfnDlgProc is a DLGPROC and returns INT_PTR -- sixty-four bits, because a
 * dialog procedure's result can be a handle -- so it takes the WIDE
 * trampoline.  pfnCallback is an LPFNPSPCALLBACK and returns UINT, so it takes
 * the narrow one.  The width is a property of the declared shape and not of
 * the values that happen to travel.
 *
 * -> FALSE if a procedure that IS there could not be wrapped, which is the
 * caller's cue to refuse the whole call rather than create a page whose dialog
 * procedure is x86-64 code. */
static BOOL guest_wrap_page( PROPSHEETPAGEA *psp )
{
    void *w;

    if (psp->dwSize < PROPSHEETPAGEA_V1_SIZE)
    {
        /* Too short to contain either field.  Native CreatePropertySheetPage
         * rejects exactly this and answers NULL, so leaving it alone gives the
         * caller comctl32's own answer and installs nothing. */
        WARN( "property sheet page with dwSize %lu, below the %u a V1 page "
              "needs; leaving it for comctl32 to reject\n",
              psp->dwSize, (UINT)PROPSHEETPAGEA_V1_SIZE );
        return TRUE;
    }
    if (psp->pfnDlgProc)
    {
        if (!(w = guest_wrap( (void *)psp->pfnDlgProc, TRUE,
                              "property sheet pfnDlgProc" ))) return FALSE;
        psp->pfnDlgProc = (DLGPROC)w;
    }
    if ((psp->dwFlags & PSP_USECALLBACK) && psp->pfnCallback)
    {
        if (!(w = guest_wrap( (void *)psp->pfnCallback, FALSE,
                              "property sheet pfnCallback" ))) return FALSE;
        psp->pfnCallback = (LPFNPSPCALLBACKA)w;
    }
    return TRUE;
}

/* The guest's page array, copied whole and wrapped page by page.
 *
 * The array is PACKED BY EACH ENTRY'S OWN dwSize and not by sizeof -- that is
 * how PropertySheetA walks it (`pByte += ((LPCPROPSHEETPAGEA)pByte)->dwSize`)
 * and how CreatePropertySheetPage copies each one (`memcpy(ppsp, page,
 * page->dwSize)`), so this copy reproduces both exactly.  Copying sizeof()
 * bytes per entry instead would read past the caller's last page and hand
 * comctl32 the wrong stride.
 *
 * The copy only has to outlive the call: PropertySheet turns every entry into
 * an HPROPSHEETPAGE with CreatePropertySheetPage, which takes its own copy,
 * before it creates any dialog -- true for PSH_MODELESS too, which returns
 * after that loop and not before it. */
static void *guest_copy_pages( const void *pages, UINT count )
{
    const BYTE *src = pages;
    DWORD total = 0;
    BYTE *copy, *p;
    UINT i;

    if (!pages || !count) return NULL;
    if (count > GUEST_PSP_MAX_PAGES)
    {
        ERR( "property sheet with %u pages, more than the %u this port will "
             "walk\n", count, GUEST_PSP_MAX_PAGES );
        return NULL;
    }
    for (i = 0; i < count; i++)
    {
        DWORD size = ((const PROPSHEETPAGEA *)(src + total))->dwSize;

        if (!size || size > GUEST_PSP_MAX_BYTES ||
            total > GUEST_PSP_MAX_BYTES - size)
        {
            ERR( "property sheet page %u declares dwSize %lu; refusing to walk "
                 "the array\n", i, size );
            return NULL;
        }
        total += size;
    }
    if (!(copy = Alloc( total ))) return NULL;
    memcpy( copy, src, total );

    for (p = copy, i = 0; i < count; i++)
    {
        PROPSHEETPAGEA *psp = (PROPSHEETPAGEA *)p;
        DWORD size = psp->dwSize;

        if (!guest_wrap_page( psp ))
        {
            Free( copy );
            return NULL;
        }
        p += size;
    }
    return copy;
}

/* Everything one wrapped PropertySheet call allocated, so that the refusal
 * path and the success path free the same things in the same place. */
struct guest_sheet
{
    void  *pages;               /* the copied ppsp blob, or NULL */
    void **loose;               /* copies of raw pages found in phpage */
    UINT   loose_count;
    HPROPSHEETPAGE *phpage;     /* the copied phpage array, or NULL */
};

static void guest_sheet_free( struct guest_sheet *s )
{
    UINT i;

    for (i = 0; i < s->loose_count; i++) Free( s->loose[i] );
    Free( s->loose );
    Free( s->phpage );
    Free( s->pages );
    memset( s, 0, sizeof(*s) );
}

/* The header, copied and wrapped.  -> FALSE means refuse the call.
 *
 * The header is the caller's `const` struct and PropertySheet never writes
 * back through it, so unlike comdlg32's dialog hooks this one IS a copy -- the
 * emu_RegisterClass case exactly.  It has to be: a guest may perfectly well
 * hand over a PROPSHEETHEADER that lives in its own .rdata.
 *
 * The copy is a FULL-SIZE zeroed struct holding the caller's own dwSize bytes,
 * capped at sizeof, which is exactly what PROPSHEET_CollectSheetInfo copies.
 * dwSize itself travels untouched, so comctl32 sees the version the caller
 * declared. */
static BOOL guest_copy_header( PROPSHEETHEADERA *out, const PROPSHEETHEADERA *in,
                               struct guest_sheet *s, const char *export_name )
{
    UINT i;
    void *w;

    memset( out, 0, sizeof(*out) );
    memset( s, 0, sizeof(*s) );

    if (in->dwSize < PROPSHEETHEADERA_V1_SIZE)
    {
        /* Below V1 the header does not reach as far as nPages, the page array
         * or pfnCallback: there is nothing this file can read without reading
         * past the caller's struct.  comctl32 reading past it anyway is
         * comctl32's own long-standing bug and not a licence to repeat it. */
        ERR( "comctl32: refusing %s for an x86-64 guest -- its PROPSHEETHEADER "
             "declares dwSize %lu, short of the %u that holds nPages, the page "
             "array and pfnCallback, so its guest procedures cannot be found\n",
             export_name, in->dwSize, (UINT)PROPSHEETHEADERA_V1_SIZE );
        return FALSE;
    }
    memcpy( out, in, min( in->dwSize, sizeof(*out) ) );

    if ((out->dwFlags & PSH_USECALLBACK) && out->pfnCallback)
    {
        /* PFNPROPSHEETCALLBACK returns INT: the narrow trampoline. */
        if (!(w = guest_wrap( (void *)out->pfnCallback, FALSE,
                              "property sheet header pfnCallback" ))) goto refuse;
        out->pfnCallback = (PFNPROPSHEETCALLBACK)w;
    }

    if (out->dwFlags & PSH_PROPSHEETPAGE)
    {
        if (out->nPages && out->ppsp)
        {
            if (!(s->pages = guest_copy_pages( out->ppsp, out->nPages ))) goto refuse;
            out->ppsp = s->pages;
        }
        return TRUE;
    }

    /* The handle array.  Entries that really are HPROPSHEETPAGEs came from
     * CreatePropertySheetPage, which this file already wrapped, and travel
     * untouched.  Wine additionally accepts a RAW page struct in this array --
     * PropertySheetA tests the magic and falls back to CreatePropertySheetPage
     * on anything else -- and such an entry has never been wrapped by anybody,
     * so it is copied and wrapped here and the copy substituted.  Native
     * comctl32's fallback then copies OUR copy, which is why these only have
     * to live until the call returns. */
    if (!out->nPages || !out->phpage) return TRUE;
    if (out->nPages > GUEST_PSP_MAX_PAGES)
    {
        ERR( "comctl32: refusing %s -- %u pages, more than the %u this port "
             "will walk\n", export_name, out->nPages, GUEST_PSP_MAX_PAGES );
        goto refuse;
    }
    if (!(s->phpage = Alloc( out->nPages * sizeof(*s->phpage) ))) goto refuse;
    memcpy( s->phpage, out->phpage, out->nPages * sizeof(*s->phpage) );
    if (!(s->loose = Alloc( out->nPages * sizeof(*s->loose) ))) goto refuse;

    for (i = 0; i < out->nPages; i++)
    {
        const void *raw = s->phpage[i];
        void *copy;

        if (!raw || *(const DWORD *)raw == HPROPSHEETPAGE_MAGIC) continue;
        if (!(copy = guest_copy_pages( raw, 1 ))) goto refuse;
        s->loose[s->loose_count++] = copy;
        s->phpage[i] = copy;
    }
    out->phpage = s->phpage;
    return TRUE;

refuse:
    ERR( "comctl32: refusing %s for an x86-64 guest -- a page or header "
         "procedure could not be swapped for a native trampoline, and native "
         "comctl32 would call x86-64 bytes as ppc64\n", export_name );
    guest_sheet_free( s );
    return FALSE;
}

INT_PTR WINAPI __wine_guest_PropertySheetA( LPCPROPSHEETHEADERA lppsh )
{
    struct guest_sheet s;
    PROPSHEETHEADERA hdr;
    INT_PTR ret;

    if (!lppsh) return PropertySheetA( lppsh );
    if (!guest_copy_header( &hdr, lppsh, &s, "PropertySheetA" )) return -1;
    ret = PropertySheetA( &hdr );
    guest_sheet_free( &s );
    return ret;
}

INT_PTR WINAPI __wine_guest_PropertySheetW( LPCPROPSHEETHEADERW lppsh )
{
    struct guest_sheet s;
    PROPSHEETHEADERW hdr;
    INT_PTR ret;

    if (!lppsh) return PropertySheetW( lppsh );
    if (!guest_copy_header( (PROPSHEETHEADERA *)&hdr,
                            (const PROPSHEETHEADERA *)lppsh, &s,
                            "PropertySheetW" )) return -1;
    ret = PropertySheetW( &hdr );
    guest_sheet_free( &s );
    return ret;
}

/* One page, copied and wrapped.  The copy is freed on return: native
 * CreatePropertySheetPage memcpy's dwSize bytes out of it into storage of its
 * own and keeps nothing else. */
HPROPSHEETPAGE WINAPI __wine_guest_CreatePropertySheetPageA( LPCPROPSHEETPAGEA page )
{
    HPROPSHEETPAGE ret;
    void *copy;

    if (!page) return CreatePropertySheetPageA( page );
    if (!(copy = guest_copy_pages( page, 1 )))
    {
        ERR( "comctl32: refusing CreatePropertySheetPageA for an x86-64 guest "
             "-- its dialog procedure could not be swapped for a native "
             "trampoline\n" );
        return NULL;
    }
    ret = CreatePropertySheetPageA( copy );
    Free( copy );
    return ret;
}

HPROPSHEETPAGE WINAPI __wine_guest_CreatePropertySheetPageW( LPCPROPSHEETPAGEW page )
{
    HPROPSHEETPAGE ret;
    void *copy;

    if (!page) return CreatePropertySheetPageW( page );
    if (!(copy = guest_copy_pages( page, 1 )))
    {
        ERR( "comctl32: refusing CreatePropertySheetPageW for an x86-64 guest "
             "-- its dialog procedure could not be swapped for a native "
             "trampoline\n" );
        return NULL;
    }
    ret = CreatePropertySheetPageW( copy );
    Free( copy );
    return ret;
}
