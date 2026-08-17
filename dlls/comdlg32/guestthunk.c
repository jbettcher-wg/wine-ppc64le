/*
 * comdlg32.dll -- the common dialogs' HOOK PROCEDURES, for an x86-64 guest.
 *
 * THE PROBLEM, AND WHY IT IS NOT THE ONE THE WNDPROC ROWS ALREADY SOLVE.
 * Every common dialog takes ONE pointer to a struct, and every one of those
 * structs has an `lpfnHook` field: an application-supplied dialog procedure
 * that comdlg32 installs on the dialog it creates and then calls for every
 * message that dialog receives.  A guest's hook procedure is x86-64 code; the
 * call comdlg32 makes is an ELFv2 `bctrl`.
 *
 * dlls/ntdll/signal_ppc64.c's override table already intercepts the
 * REGISTRATION of a window procedure -- RegisterClass{,Ex}{A,W},
 * SetWindowLongPtr(GWLP_WNDPROC), CallWindowProc -- and that covers a guest
 * that creates its own windows.  It does NOT cover this: the pointer is not
 * an argument (no argument-position mask can name a field inside a struct)
 * and it never passes through RegisterClass, because the dialog class is
 * comdlg32's own and the hook is chained by comdlg32's own procedure.
 *
 * WHAT THIS FILE DOES NOW.  spec2thunk's GUEST-IMPL redirect (see
 * comdlg32.thunks) points each GUEST export here.  Each wrapper looks at the
 * caller's own enabling flag, and:
 *
 *   * flag clear -- the overwhelmingly common case, and what a launcher's
 *     "choose install directory" does -- calls the real export unchanged.
 *     There is no guest pointer to wrap, so nothing is touched;
 *
 *   * flag set -- swaps the hook for one of ntdll's guest-callback
 *     trampolines (__wine_guest_wrap_callback) and calls the real export.
 *     Native comdlg32 then calls a NATIVE function pointer, which re-enters
 *     the guest through the port's own run-entry primitive.
 *
 * These wrappers used to refuse the second case by name.  The refusal is gone;
 * what is left of it is the one honest failure path below, taken when ntdll
 * has no trampoline factory at all or cannot mint a stub.
 *
 * IN PLACE, AND NOT A COPY -- the one place this differs from
 * emu_RegisterClass in signal_ppc64.c, which copies WNDCLASS before swapping
 * lpfnWndProc.  Three reasons, all properties of THESE structs rather than
 * preferences:
 *
 *   1. the struct is an IN/OUT parameter.  GetOpenFileName writes nFileOffset,
 *      nFileExtension, nFilterIndex and Flags back into it; ChooseColor writes
 *      rgbResult; PrintDlg writes hDevMode and hDevNames.  A copy would have to
 *      be copied back, and a copy-back is a second place for the field list to
 *      drift.  WNDCLASS is const and is never written back, which is exactly
 *      why copying is right there and not here.
 *
 *   2. comdlg32 hands the hook THE CALLER'S OWN STRUCT POINTER: an
 *      OFNHOOKPROC's WM_INITDIALOG arrives with lParam = the OPENFILENAME the
 *      application passed, and reading lCustData out of it is the standard
 *      idiom.  Pass a copy and the guest's hook is handed an address the guest
 *      does not recognise.
 *
 *   3. FindText and ReplaceText are MODELESS.  They return an HWND
 *      immediately and the dialog goes on calling lpfnHook -- read out of the
 *      caller's FINDREPLACE, which the application is required to keep alive
 *      -- for as long as it lives.  A swap that was undone on return would
 *      leave that dialog calling x86-64 bytes on the next keystroke.
 *
 * So the swap is permanent, and that is safe because the trampoline pool is
 * idempotent and deduplicating: the same guest hook always yields the same
 * trampoline, and a trampoline handed back in yields itself.  A guest that
 * reuses one OPENFILENAME for a hundred dialogs mints one stub.  What a guest
 * READS BACK from lpfnHook afterwards is the trampoline, which is the same
 * answer GetWindowLongPtr(GWLP_WNDPROC) already gives on this port, for the
 * same reason and with the same consequence: the trampoline IS the procedure
 * as far as native code is concerned.
 *
 * WIDTH.  Every hook here returns UINT_PTR -- a full 64 bits, because a
 * dialog procedure's result can be a handle or a pointer -- and so takes the
 * WIDE trampoline.  The two exceptions are PageSetupDlg's, which Wine's
 * commdlg.h declares as returning UINT: those get the narrow, sign-extended
 * form, because the width is a property of the declared shape and not of what
 * the values happen to be.
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

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "commdlg.h"
#include "cderr.h"

#include "cdlg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(guestcb);

/* ------------------------------------------------- ntdll's callback factory
 *
 * Resolved by name at first use rather than linked against, which is the
 * resolve_gl_entry_point discipline in dlls/opengl32/wgl.c and the same one
 * dlls/dinput8/guestcom.c keeps: a tree whose ntdll predates the export
 * refuses HERE, loudly and by name, instead of failing to load comdlg32 at
 * all and taking every dialog down with it. */
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
        ERR( "comdlg32: this ntdll exports no __wine_guest_wrap_callback; a "
             "hook-enabled common dialog cannot swap the application's hook "
             "procedure for a trampoline and will refuse rather than let "
             "native comdlg32 call x86-64 bytes\n" );
        return FALSE;
    }
    guest_wrap_callback = proc;
    return TRUE;
}

/* COMDLG32_SetCommDlgExtendedError (cdlg.h, implemented in cdlg32.c) is this
 * module's own last-error slot, so a refused call answers through the channel
 * CommDlgExtendedError() reads and the caller's existing error path works
 * unchanged.
 *
 * CDERR_INITIALIZATION is what comdlg32 itself returns when it cannot set a
 * dialog up.  It is the honest code here: the dialog genuinely cannot be
 * initialised the way this caller asked for.
 *
 * This is now reached only when the trampoline factory is missing or refuses
 * -- never merely because a hook was requested. */
static BOOL guest_refuse_hook( const char *dialog, const char *flag )
{
    ERR( "comdlg32: refusing %s for an x86-64 guest -- it sets %s and the hook "
         "procedure inside its struct could not be swapped for a native "
         "trampoline.  Reporting CDERR_INITIALIZATION rather than letting "
         "native comdlg32 execute x86-64 bytes as ppc64\n", dialog, flag );
    COMDLG32_SetCommDlgExtendedError( CDERR_INITIALIZATION );
    return FALSE;
}

/* Swap one hook field in place.  FALSE means "could not", and the caller
 * refuses; a NULL field is TRUE and left alone, because a caller that sets the
 * enabling flag with no procedure is comdlg32's error to report and not
 * ours. */
static BOOL guest_wrap_hook( void **field, const char *dialog, BOOL wide )
{
    void *wrapped;

    if (!*field) return TRUE;
    if (!resolve_wrap_callback()) return FALSE;
    if (!(wrapped = guest_wrap_callback( *field, wide )))
    {
        ERR( "comdlg32: %s: the trampoline pool would not mint a stub for hook "
             "procedure %p\n", dialog, *field );
        return FALSE;
    }
    TRACE( "%s: hook %p -> trampoline %p\n", dialog, *field, wrapped );
    *field = wrapped;
    return TRUE;
}

/* The wrappers.  The real exports are declared by commdlg.h, which this
 * module compiles with WINCOMMDLGAPI decoration -- redeclaring one here
 * would disagree with that and is not needed. */

#define FILE_DLG(name, type)                                                 \
    BOOL WINAPI __wine_guest_##name( type *ofn )                             \
    {                                                                        \
        if (ofn && (ofn->Flags & OFN_ENABLEHOOK) &&                          \
            !guest_wrap_hook( (void **)&ofn->lpfnHook, #name, TRUE ))        \
            return guest_refuse_hook( #name, "OFN_ENABLEHOOK" );             \
        return name( ofn );                                                  \
    }

FILE_DLG( GetOpenFileNameA, OPENFILENAMEA )
FILE_DLG( GetOpenFileNameW, OPENFILENAMEW )
FILE_DLG( GetSaveFileNameA, OPENFILENAMEA )
FILE_DLG( GetSaveFileNameW, OPENFILENAMEW )

#define COLOR_DLG(name, type)                                                \
    BOOL WINAPI __wine_guest_##name( type *cc )                              \
    {                                                                        \
        if (cc && (cc->Flags & CC_ENABLEHOOK) &&                             \
            !guest_wrap_hook( (void **)&cc->lpfnHook, #name, TRUE ))         \
            return guest_refuse_hook( #name, "CC_ENABLEHOOK" );              \
        return name( cc );                                                   \
    }

COLOR_DLG( ChooseColorA, CHOOSECOLORA )
COLOR_DLG( ChooseColorW, CHOOSECOLORW )

#define FONT_DLG(name, type)                                                 \
    BOOL WINAPI __wine_guest_##name( type *cf )                              \
    {                                                                        \
        if (cf && (cf->Flags & CF_ENABLEHOOK) &&                             \
            !guest_wrap_hook( (void **)&cf->lpfnHook, #name, TRUE ))         \
            return guest_refuse_hook( #name, "CF_ENABLEHOOK" );              \
        return name( cf );                                                   \
    }

FONT_DLG( ChooseFontA, CHOOSEFONTA )
FONT_DLG( ChooseFontW, CHOOSEFONTW )

/* FindText and ReplaceText are the modeless pair: the swap below outlives the
 * call by design (see the banner's reason 3). */
#define FINDREPLACE_DLG(name, type)                                          \
    HWND WINAPI __wine_guest_##name( type *fr )                              \
    {                                                                        \
        if (fr && (fr->Flags & FR_ENABLEHOOK) &&                             \
            !guest_wrap_hook( (void **)&fr->lpfnHook, #name, TRUE ))         \
        {                                                                    \
            guest_refuse_hook( #name, "FR_ENABLEHOOK" );                     \
            return NULL;                                                     \
        }                                                                    \
        return name( fr );                                                   \
    }

FINDREPLACE_DLG( FindTextA, FINDREPLACEA )
FINDREPLACE_DLG( FindTextW, FINDREPLACEW )
FINDREPLACE_DLG( ReplaceTextA, FINDREPLACEA )
FINDREPLACE_DLG( ReplaceTextW, FINDREPLACEW )

/* PrintDlg carries TWO hooks -- one for the print dialog, one for the printer
 * setup dialog it can put up instead -- and each has its own enabling flag.
 * Both are wrapped before the call, so a dialog that switches from one to the
 * other mid-flight finds a trampoline either way. */
#define PRINT_DLG(name, type)                                                \
    BOOL WINAPI __wine_guest_##name( type *pd )                              \
    {                                                                        \
        if (pd && (pd->Flags & PD_ENABLEPRINTHOOK) &&                        \
            !guest_wrap_hook( (void **)&pd->lpfnPrintHook, #name, TRUE ))    \
            return guest_refuse_hook( #name, "PD_ENABLEPRINTHOOK" );         \
        if (pd && (pd->Flags & PD_ENABLESETUPHOOK) &&                        \
            !guest_wrap_hook( (void **)&pd->lpfnSetupHook, #name, TRUE ))    \
            return guest_refuse_hook( #name, "PD_ENABLESETUPHOOK" );         \
        return name( pd );                                                   \
    }

PRINT_DLG( PrintDlgA, PRINTDLGA )
PRINT_DLG( PrintDlgW, PRINTDLGW )

/* PageSetupDlg carries two as well, and the second one -- the page-paint hook
 * -- is called once per WM_PSD_* message while the sample page is drawn.
 * Both are NARROW: commdlg.h declares LPPAGESETUPHOOK and LPPAGEPAINTHOOK as
 * returning UINT, not the UINT_PTR every other hook in this file returns, so
 * native comdlg32 reads a sign-extended 32-bit result from them. */
#define PAGESETUP_DLG(name, type)                                            \
    BOOL WINAPI __wine_guest_##name( type *psd )                             \
    {                                                                        \
        if (psd && (psd->Flags & PSD_ENABLEPAGESETUPHOOK) &&                 \
            !guest_wrap_hook( (void **)&psd->lpfnPageSetupHook, #name,       \
                              FALSE ))                                       \
            return guest_refuse_hook( #name, "PSD_ENABLEPAGESETUPHOOK" );    \
        if (psd && (psd->Flags & PSD_ENABLEPAGEPAINTHOOK) &&                 \
            !guest_wrap_hook( (void **)&psd->lpfnPagePaintHook, #name,       \
                              FALSE ))                                       \
            return guest_refuse_hook( #name, "PSD_ENABLEPAGEPAINTHOOK" );    \
        return name( psd );                                                  \
    }

PAGESETUP_DLG( PageSetupDlgA, PAGESETUPDLGA )
PAGESETUP_DLG( PageSetupDlgW, PAGESETUPDLGW )

/* PrintDlgEx is different in kind, and refused unconditionally: PRINTDLGEX
 * carries `IPrintDialogCallback *lpCallback`, an interface the APPLICATION
 * implements, and hands it to native code.  A trampoline cannot help -- a
 * trampoline wraps one function pointer, and what crosses here is a vtable of
 * them -- so this is the reverse-proxy direction winecom does not have
 * (system-com-design.md §6, step 5).  There is no flag to test.  It also
 * RETURNS an HRESULT rather than a BOOL, so the refusal is E_NOTIMPL. */
HRESULT WINAPI __wine_guest_PrintDlgExA( PRINTDLGEXA *pdex )
{
    ERR( "comdlg32: refusing PrintDlgExA for an x86-64 guest -- PRINTDLGEX "
         "carries an IPrintDialogCallback the application implements, and a "
         "guest-implemented interface handed to native code needs a reverse "
         "proxy, which a callback trampoline is not\n" );
    return E_NOTIMPL;
}

HRESULT WINAPI __wine_guest_PrintDlgExW( PRINTDLGEXW *pdex )
{
    ERR( "comdlg32: refusing PrintDlgExW for an x86-64 guest -- PRINTDLGEX "
         "carries an IPrintDialogCallback the application implements, and a "
         "guest-implemented interface handed to native code needs a reverse "
         "proxy, which a callback trampoline is not\n" );
    return E_NOTIMPL;
}
