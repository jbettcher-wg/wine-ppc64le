/*
 * wininet.dll -- the callback an x86-64 guest hands to native wininet.
 *
 * InternetSetStatusCallbackA/W hand native wininet an INTERNET_STATUS_CALLBACK
 * that wininet retains for the life of the handle and calls from its own
 * worker threads -- once per resolve/connect/redirect/receive step, on a
 * thread the guest never entered.  A guest function pointer stored there is
 * x86-64 code reached by an ELFv2 bctrl: the same shape as the FONTENUMPROC
 * and MONITORENUMPROC failures dlls/ntdll/signal_ppc64.c's override table
 * records, and worse than either, because it fires asynchronously and there
 * is no guest frame anywhere on the stack to blame.
 *
 * THIS FILE USED TO NOT EXIST, because the callback is FIVE arguments --
 *
 *     typedef VOID (CALLBACK *INTERNET_STATUS_CALLBACK)(
 *         HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
 *         LPVOID lpvStatusInformation, DWORD dwStatusInformationLength );
 *
 * -- and ntdll's trampoline used to carry only four.  The fifth argument here
 * is dwStatusInformationLength, and it is not decoration:
 * INTERNET_STATUS_HANDLE_CREATED, _REQUEST_COMPLETE and _REDIRECT all
 * describe the SIZE of what lpvStatusInformation points at, and a callback
 * that trusted a garbage length would read or copy an arbitrary amount of
 * memory.  Wrapping with the four-argument trampoline would have turned a
 * loud missing import into a silent wrong answer on a worker thread -- the
 * single failure mode this whole registration-side mechanism exists to
 * remove.
 *
 * ntdll now exports __wine_guest_wrap_callback5, the five-argument sibling of
 * __wine_guest_wrap_callback (see call_guest_function_args5 in
 * dlls/ntdll/signal_ppc64.c, which builds the extra stack slot MS-x64 passes
 * dwStatusInformationLength in), and this file resolves it the same way
 * dlls/comctl32/guestthunk.c resolves the four- and six-argument factories.
 * INTERNET_STATUS_CALLBACK returns void, so the WIDE/narrow choice does not
 * matter for correctness; the narrow trampoline is used, matching every other
 * void-returning callback this port wraps.
 *
 * THE RETURN of InternetSetStatusCallback is the PREVIOUSLY registered
 * callback, which for a guest that registers twice is one of our own
 * trampolines.  Handing it back is right -- the pool is idempotent, so
 * re-registering the same guest function maps to the same trampoline -- and
 * it is the same answer SetUnhandledExceptionFilter already gives on this
 * port.  A guest that CALLS the returned pointer rather than re-registering
 * it would execute ppc64 bytes as x86-64, which is the accepted residual risk
 * everywhere this port hands a trampoline back to a guest.
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
#include "winternl.h"
#include "wininet.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(guestcb);

/* ------------------------------------------------- ntdll's callback factory
 *
 * Resolved by name at first use rather than linked against -- the
 * resolve_gl_entry_point discipline in dlls/opengl32/wgl.c, and the same one
 * dlls/comctl32/guestthunk.c, dlls/dinput8/guestcom.c and
 * dlls/comdlg32/guestthunk.c keep.  A tree whose ntdll predates the export
 * refuses HERE, loudly and by name, rather than failing to load wininet and
 * taking every network call down with it. */
static void *(CDECL *guest_wrap_callback5)( void *fn, BOOL wide );
/* PUBLICATION IS THE POINTER ITSELF, not a separate "have we looked yet"
 * flag.  The flag form -- InterlockedCompareExchange(&resolved, 1, 0) and then
 * `return ptr != NULL` -- has a window: the thread that WINS the exchange is
 * still inside LdrGetProcedureAddress when a second thread arrives, sees the
 * flag already set, reads a pointer that has not been stored yet, and reports
 * "this ntdll has no such export" about an ntdll that does.  The caller then
 * refuses a callback it could have served, on a race, once, and never again
 * for the life of the process -- which is exactly the kind of failure that
 * gets blamed on the guest.  Resolving twice costs two name lookups and
 * publishes the same address, so the lookup is simply repeated until it
 * succeeds; wrap5_missing remembers a genuine absence so an old ntdll does
 * not pay a loader walk on every call, and the ERR is said once. */
static LONG wrap5_missing, wrap5_said;

static BOOL resolve_wrap_callback5(void)
{
    UNICODE_STRING ntdllW;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    if (guest_wrap_callback5) return TRUE;
    if (wrap5_missing) return FALSE;

    RtlInitUnicodeString( &ntdllW, L"ntdll.dll" );
    RtlInitAnsiString( &name, "__wine_guest_wrap_callback5" );
    if (LdrGetDllHandle( NULL, 0, &ntdllW, &ntdll ) ||
        LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        if (!InterlockedExchange( &wrap5_said, 1 ))
            ERR( "wininet: this ntdll exports no __wine_guest_wrap_callback5; a "
                 "guest INTERNET_STATUS_CALLBACK cannot be swapped for a "
                 "trampoline, and InternetSetStatusCallbackA/W will refuse rather "
                 "than let native wininet call x86-64 bytes from a worker "
                 "thread\n" );
        InterlockedExchange( &wrap5_missing, 1 );
        return FALSE;
    }
    InterlockedExchangePointer( (void **)&guest_wrap_callback5, proc );
    return TRUE;
}

/* -> the trampoline, or NULL if there is none to be had.  NULL in is NULL out
 * and not a failure: INTERNET_INVALID_STATUS_CALLBACK / no callback at all is
 * a guest unregistering, and native InternetSetStatusCallbackA/W already
 * treats a NULL lpfnInternetCallback as exactly that. */
static void *guest_wrap5( void *fn, const char *what )
{
    void *wrapped;

    if (!fn) return NULL;
    if (!resolve_wrap_callback5()) return NULL;
    if (!(wrapped = guest_wrap_callback5( fn, FALSE )))
    {
        ERR( "wininet: the trampoline pool would not mint a five-argument "
             "stub for %s %p\n", what, fn );
        return NULL;
    }
    TRACE( "%s %p -> five-argument trampoline %p\n", what, fn, wrapped );
    return wrapped;
}

/* Both forms: wrap the guest's callback, register the trampoline, and hand
 * back whatever native wininet hands back -- the previously registered
 * callback (possibly one of our own trampolines; see the banner above) or
 * INTERNET_INVALID_STATUS_CALLBACK on failure.  A guest callback that could
 * not be wrapped is refused the same way -- the trampoline pool already logs
 * why -- rather than let a raw guest pointer reach a native worker thread. */
INTERNET_STATUS_CALLBACK WINAPI __wine_guest_InternetSetStatusCallbackA(
    HINTERNET hInternet, INTERNET_STATUS_CALLBACK lpfnInternetCallback )
{
    void *wrapped;

    if (!lpfnInternetCallback)
        return InternetSetStatusCallbackA( hInternet, NULL );
    if (!(wrapped = guest_wrap5( (void *)lpfnInternetCallback,
                                 "InternetSetStatusCallbackA callback" )))
        return INTERNET_INVALID_STATUS_CALLBACK;
    return InternetSetStatusCallbackA( hInternet, (INTERNET_STATUS_CALLBACK)wrapped );
}

INTERNET_STATUS_CALLBACK WINAPI __wine_guest_InternetSetStatusCallbackW(
    HINTERNET hInternet, INTERNET_STATUS_CALLBACK lpfnInternetCallback )
{
    void *wrapped;

    if (!lpfnInternetCallback)
        return InternetSetStatusCallbackW( hInternet, NULL );
    if (!(wrapped = guest_wrap5( (void *)lpfnInternetCallback,
                                 "InternetSetStatusCallbackW callback" )))
        return INTERNET_INVALID_STATUS_CALLBACK;
    return InternetSetStatusCallbackW( hInternet, (INTERNET_STATUS_CALLBACK)wrapped );
}
