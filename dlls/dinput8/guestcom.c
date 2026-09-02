/*
 * DirectInput8 for x86-64 guests on native ppc64le Wine -- the dinput8-side
 * winecom runtime instance and the flat-export wrapper.
 *
 * This is the native half of the dinput8 COM boundary, built the same way as
 * dlls/combase/syscom.c (hangover-ppc64le/docs/system-com-design.md).  Nothing
 * here replaces any Wine implementation: DirectInput8Create, the
 * IDirectInput8A/W objects it vends and the IDirectInputDevice8A/W objects
 * those vend are all Wine's own, in this same module, compiled for ppc64.  The
 * only thing that is wrong without this file is where an INTERFACE POINTER
 * ends up:
 *
 *   DirectInput8Create writes an IDirectInput8A* through its `out` argument.
 *   Handed to an x86-64 guest unchanged, that is a NATIVE ppc64 vtable, and
 *   the guest's very next line -- IDirectInput8_CreateDevice, always -- is an
 *   x86-64 `call [rax+0x18]` into ppc64 bytes.  DirectInput is the FIRST thing
 *   a game touches after it has a window, so this is not a corner.
 *
 * WHAT THIS FILE IS:
 *
 *   * THE runtime instance for this module's surface.  libs/winecom state is
 *     per-linkee, deliberately, so dinput8's instance is separate from
 *     combase's and from d3d11's and no proxy is ever confused between them
 *     (design §4.2).  The roster is ppc64le/shell/interfaces_dinput.json --
 *     the same file dinput8.thunks hands spec2thunk to build the guest trap
 *     module -- turned into the tables below by
 *     ppc64le/shell/gen_dinput_surface.py.
 *
 *   * the host invoker: a DIRECT widest-form native vtable call, exactly
 *     combase's.  There is no unixlib on this surface -- the implementations
 *     are ordinary native PE code in this very DLL.
 *
 *   * __wine_guest_DirectInput8Create, the flat wrapper the guest's
 *     DirectInput8Create export resolves to (spec2thunk GUEST-IMPL, §4.3).
 *     The plain DirectInput8Create export is untouched and still serves
 *     native ppc64 callers.
 *
 * WHAT IS REFUSED, AND WHERE IT IS SAID: every method of every interface here
 * whose signature carries a CALLBACK -- EnumDevices, EnumDevicesBySemantics,
 * ConfigureDevices, EnumObjects, EnumEffects, EnumCreatedEffectObjects,
 * EnumEffectsInFile, in both character widths, fourteen slots -- carries a
 * `refuse` string in the generated table, so winecom answers E_NOTIMPL and
 * FIXMEs the reason ONCE, naming the interface and the method.  A game that
 * enumerates gamepads gets a clean empty enumeration failure it can report,
 * not a c000001d on a native thread.  Keyboard and mouse do not enumerate:
 * CreateDevice(GUID_SysKeyboard) / CreateDevice(GUID_SysMouse), SetDataFormat,
 * SetCooperativeLevel, Acquire, GetDeviceState and GetDeviceData are all
 * marshalled and all work.
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

#define COBJMACROS   /* the 2026-09-01 shims AddRef what they wrap */

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "winternl.h"

#include "dinput.h"

#include "wine/debug.h"
#include "wine/winecom.h"

#include "dinput8_marshal.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

/* ------------------------------------------------------- the host invoker */

/* A direct widest-form native vtable call: host's vtable slot with up to 16
 * ULONG_PTR arguments (args[0] is `this`).  ELFv2 callees ignore the excess,
 * so one shape serves every slot -- the same trick call_native_thunk uses, and
 * the same invoker dlls/combase/syscom.c has, for the same reason: these are
 * ordinary native COM objects in this process, not something behind a unixlib.
 * The widest slot in this surface takes six arguments. */
static UINT64 dinput8_invoke( void *host, UINT slot, UINT argc, UINT64 *args )
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

/* The guest module whose published stub arrays become the proxies' vtables.
 * Only this one: dinput.dll (DirectInput 7) is a SEPARATE module with its own
 * guest thunk and no COM roster, and listing it here would have winecom
 * cross-check IIDs against a module that publishes none.  See
 * dlls/dinput/dinput.thunks. */
static const WCHAR *const dinput8_guest_modules[] = { L"dinput8.dll" };

/* ------------------------------------------------------ hand-written slots
 *
 * THE ENUMERATIONS.  Every Enum* method on this surface takes a bare GUEST
 * FUNCTION POINTER that native dinput retains for the duration of the call and
 * invokes once per item, from a native frame.  That is the reverse direction
 * in its oldest form -- and the one form a REVERSE PROXY does not close, since
 * a reverse proxy is a vtable and a DIENUMDEVICESCALLBACK has none.
 *
 * The port's answer for a bare callback is older than winecom and lives in
 * dlls/ntdll/signal_ppc64.c: a pool of native trampolines, one per distinct
 * guest target, swapped in at REGISTRATION by whoever knows the argument is a
 * callback.  A flat export gets that from ntdll's own override table, keyed on
 * the export name; a COM METHOD cannot, because it traps inside a vtable stub
 * array and is routed by RIP arithmetic, so the table can never see it.  That
 * is precisely why ntdll exports __wine_guest_wrap_callback -- and these are
 * the slots the export was added for.
 *
 * SERVED here are the four shapes whose callback receives PLAIN DATA plus the
 * caller's own pvRef: EnumDevices, EnumObjects, EnumEffects and
 * EnumEffectsInFile, in both character widths.  A trampoline carries its
 * arguments through untranslated, which is exactly right for a
 * DIDEVICEINSTANCE (both sides compile it from this same header, and guest
 * memory IS host memory here).
 *
 * SERVED SINCE 2026-09-01 (the completeness pass) are the three whose
 * callback receives an INTERFACE POINTER -- EnumDevicesBySemantics gets an
 * LPDIRECTINPUTDEVICE8, EnumCreatedEffectObjects an LPDIRECTINPUTEFFECT,
 * ConfigureDevices an IUnknown draw surface.  Each has the per-callback SHIM
 * the old refusal text said serving one needs: a native function takes the
 * callback's place, wraps that argument as a proxy, and enters the guest
 * through the same trampoline the plain enumerations use (the five-argument
 * semantics callback through __wine_guest_wrap_callback5).  See the shim
 * block below the plain hands.
 */

/* ntdll's trampoline factory, resolved once by name.  A tree whose ntdll
 * predates the export refuses loudly here rather than failing to load, which
 * is the same discipline the rest of this file keeps. */
static void *(CDECL *guest_wrap_callback)( void *fn, BOOL wide );
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
 * succeeds; wrap_missing remembers a genuine absence so an old ntdll does
 * not pay a loader walk on every call, and the ERR is said once. */
static LONG wrap_missing, wrap_said;

static BOOL resolve_wrap_callback(void)
{
    UNICODE_STRING ntdllW;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    if (guest_wrap_callback) return TRUE;
    if (wrap_missing) return FALSE;

    RtlInitUnicodeString( &ntdllW, L"ntdll.dll" );
    RtlInitAnsiString( &name, "__wine_guest_wrap_callback" );
    if (LdrGetDllHandle( NULL, 0, &ntdllW, &ntdll ) ||
        LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        if (!InterlockedExchange( &wrap_said, 1 ))
            ERR( "dinput8: this ntdll exports no __wine_guest_wrap_callback; the "
                 "Enum* slots cannot swap a guest callback for a trampoline and "
                 "will refuse rather than let native dinput call x86-64 bytes\n" );
        InterlockedExchange( &wrap_missing, 1 );
        return FALSE;
    }
    InterlockedExchangePointer( (void **)&guest_wrap_callback, proc );
    return TRUE;
}

/* One enumeration, with the guest's callback swapped for a trampoline.
 * `cb_arg` is the callback's argument position counting `this` as 0, and it
 * comes from the generated table's own HAND_SLOTS entry, which the generator
 * checks against the signature -- so a parameter list that moved stops
 * generation rather than wrapping the wrong argument here. */
static UINT64 hand_enum( void *host, UINT slot, AMD64_CONTEXT *ctx,
                         UINT cb_arg, UINT argc )
{
    UINT64 args[16] = { 0 };
    void *cb;
    UINT i;

    cb = (void *)(ULONG_PTR)winecom_read_arg( ctx, cb_arg );
    if (cb)
    {
        if (!resolve_wrap_callback()) return (UINT64)(UINT)E_NOTIMPL;
        /* The NARROW form: a DIENUMCALLBACK returns BOOL, and an ELFv2 caller
         * is entitled to a sign-extended 32-bit result -- DIENUM_STOP is 0 and
         * DIENUM_CONTINUE is 1, so the width would not bite here, but the rule
         * is the callback's shape and not what its constants happen to be. */
        if (!(cb = guest_wrap_callback( cb, FALSE )))
        {
            ERR( "dinput8: could not wrap the guest enumeration callback; "
                 "refusing %s\n", dinput8_com_ifaces[0].name );
            return (UINT64)(UINT)E_NOTIMPL;
        }
    }
    for (i = 1; i < argc; i++) args[i] = winecom_read_arg( ctx, i );
    args[cb_arg] = (UINT64)(ULONG_PTR)cb;
    return dinput8_invoke( host, slot, argc, args );
}

/* (this, DWORD, callback, LPVOID, DWORD) -- EnumDevices and EnumEffectsInFile,
 * which differ in what their first argument means and in nothing this file
 * has to know about. */
static UINT64 hand_enum_cb2( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_enum( host, slot, ctx, 2, 5 );
}

/* (this, callback, LPVOID, DWORD) -- EnumObjects and EnumEffects. */
static UINT64 hand_enum_cb1( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_enum( host, slot, ctx, 1, 4 );
}

/* ------------------------------------------- shimmed callbacks (2026-09-01)
 *
 * The three enumerations whose callback receives an INTERFACE POINTER.  The
 * refusal these replace said exactly what serving one takes: "a per-callback
 * shim that wraps its argument as a proxy first".  These are those shims:
 * native functions handed to dinput IN PLACE of the guest's callback, with a
 * context block riding in pvRef.  Each wraps its interface argument through
 * this surface's own winecom instance, then enters the guest through the
 * SAME trampoline machinery hand_enum uses.
 *
 * REFERENCES: the device/effect dinput hands a callback is LENT for the
 * call's duration (DirectInput's own contract: AddRef to keep).  winecom_wrap
 * CONSUMES one reference, so the shim takes one first -- dinput's own
 * release balance is untouched, and the interned proxy owns what it holds.
 * The same device re-enumerated finds the same proxy (interning), which is
 * also what keeps repeated enumerations from minting garbage. */

struct dinput8_shim_ctx
{
    void *tramp;        /* the guest callback, behind a native trampoline */
    void *guest_pvref;  /* the guest's own context argument, untouched */
    UINT  iface;        /* what to wrap the interface argument as */
};

static BOOL CALLBACK dinput8_semantics_shim( const void *lpddi, void *lpdid,
                                             DWORD flags, DWORD remaining,
                                             void *ctx_ptr )
{
    struct dinput8_shim_ctx *sc = ctx_ptr;
    BOOL (CALLBACK *cb)( const void *, void *, DWORD, DWORD, void * ) = sc->tramp;
    void *proxy = NULL;

    if (lpdid)
    {
        IUnknown_AddRef( (IUnknown *)lpdid );      /* wrap consumes one */
        proxy = winecom_wrap( lpdid, sc->iface );
    }
    return cb( lpddi, proxy, flags, remaining, sc->guest_pvref );
}

static BOOL CALLBACK dinput8_iface_arg0_shim( void *itf, void *ctx_ptr )
{
    struct dinput8_shim_ctx *sc = ctx_ptr;
    BOOL (CALLBACK *cb)( void *, void * ) = sc->tramp;
    void *proxy = NULL;

    if (itf)
    {
        IUnknown_AddRef( (IUnknown *)itf );
        proxy = winecom_wrap( itf, sc->iface );
    }
    return cb( proxy, sc->guest_pvref );
}

/* ntdll's five-argument factory, resolved beside the four-argument one --
 * the semantics callback carries five. */
static void *(CDECL *guest_wrap_callback5)( void *fn, BOOL wide );

static BOOL resolve_wrap_callback5(void)
{
    UNICODE_STRING ntdllW;
    ANSI_STRING name;
    HMODULE ntdll;
    void *proc;

    if (guest_wrap_callback5) return TRUE;

    RtlInitUnicodeString( &ntdllW, L"ntdll.dll" );
    RtlInitAnsiString( &name, "__wine_guest_wrap_callback5" );
    if (LdrGetDllHandle( NULL, 0, &ntdllW, &ntdll ) ||
        LdrGetProcedureAddress( ntdll, &name, 0, &proc ))
    {
        ERR( "dinput8: this ntdll exports no __wine_guest_wrap_callback5; "
             "EnumDevicesBySemantics refuses rather than truncate the "
             "callback's five arguments\n" );
        return FALSE;
    }
    InterlockedExchangePointer( (void **)&guest_wrap_callback5, proc );
    return TRUE;
}

/* One shimmed enumeration: the guest callback becomes a trampoline, the
 * NATIVE shim takes its place, and the context block rides in pvRef.  The
 * block lives on this frame -- every one of these calls is synchronous
 * (an enumeration, or ConfigureDevices' modal UI), so it outlives every
 * shim invocation by construction. */
static UINT64 hand_shimmed_enum( void *host, UINT slot, AMD64_CONTEXT *ctx,
                                 UINT cb_arg, UINT pvref_arg, UINT argc,
                                 void *shim, UINT iface, BOOL five )
{
    UINT64 args[16] = { 0 };
    struct dinput8_shim_ctx sc;
    void *cb;
    UINT i;

    for (i = 1; i < argc; i++) args[i] = winecom_read_arg( ctx, i );
    cb = (void *)(ULONG_PTR)args[cb_arg];
    if (cb)
    {
        if (!resolve_wrap_callback()) return (UINT64)(UINT)E_NOTIMPL;
        if (five && !resolve_wrap_callback5()) return (UINT64)(UINT)E_NOTIMPL;
        sc.tramp = five ? guest_wrap_callback5( cb, FALSE )
                        : guest_wrap_callback( cb, FALSE );
        if (!sc.tramp)
        {
            ERR( "dinput8: could not wrap the guest callback; refusing\n" );
            return (UINT64)(UINT)E_NOTIMPL;
        }
        sc.guest_pvref = (void *)(ULONG_PTR)args[pvref_arg];
        sc.iface = iface;
        args[cb_arg] = (UINT64)(ULONG_PTR)shim;
        args[pvref_arg] = (UINT64)(ULONG_PTR)&sc;
    }
    return dinput8_invoke( host, slot, argc, args );
}

/* EnumDevicesBySemantics(user, actionformat, cb@2, pvRef@3, flags): the
 * DEVICE is the callback's second argument. */
static UINT64 hand_enum_semantics_a( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_shimmed_enum( host, slot, ctx, 3, 4, 6,
                              (void *)dinput8_semantics_shim,
                              DINPUT8_IFACE_IDirectInputDevice8A, TRUE );
}

static UINT64 hand_enum_semantics_w( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_shimmed_enum( host, slot, ctx, 3, 4, 6,
                              (void *)dinput8_semantics_shim,
                              DINPUT8_IFACE_IDirectInputDevice8W, TRUE );
}

/* ConfigureDevices(cb@0, params, flags, pvRef@3): the callback's first
 * argument is an IUnknown draw surface. */
static UINT64 hand_configure_devices( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_shimmed_enum( host, slot, ctx, 1, 4, 5,
                              (void *)dinput8_iface_arg0_shim,
                              DINPUT8_IFACE_IUnknown, FALSE );
}

/* EnumCreatedEffectObjects(cb@0, pvRef@1, flags): the EFFECT is the
 * callback's first argument. */
static UINT64 hand_enum_created_fx( void *host, UINT slot, AMD64_CONTEXT *ctx )
{
    return hand_shimmed_enum( host, slot, ctx, 1, 2, 4,
                              (void *)dinput8_iface_arg0_shim,
                              DINPUT8_IFACE_IDirectInputEffect, FALSE );
}

/* The order here IS the hand_funcs[] order in dinput8_marshal.h. */
static const winecom_hand_fn dinput8_hand_funcs[] =
{
    hand_enum_cb2,
    hand_enum_cb1,
    hand_enum_semantics_a,
    hand_enum_semantics_w,
    hand_configure_devices,
    hand_enum_created_fx,
};

C_ASSERT( ARRAYSIZE(dinput8_hand_funcs) == DINPUT8_HAND_COUNT );

static const struct winecom_surface dinput8_surface =
{
    .name = "dinput8",
    .guest_modules = dinput8_guest_modules,
    .module_count = ARRAYSIZE(dinput8_guest_modules),
    .ifaces = dinput8_com_ifaces,
    .iface_count = DINPUT8_IFACE_COUNT,
    .invoke = dinput8_invoke,
    .hand_funcs = dinput8_hand_funcs,
    .hand_count = DINPUT8_HAND_COUNT,
    /* NOT WINECOM_SF_REVERSE, and that is a decision rather than an omission.
     * The only interface pointer a guest hands INTO this surface is
     * aggregation's pUnkOuter, and Wine's own DirectInput8Create answers
     * DIERR_NOAGGREGATION for any non-NULL one -- so a reverse proxy here
     * would be machinery with nothing behind it.  The enumerations, which ARE
     * the reverse direction on this surface, are bare function pointers and
     * are served by the trampoline pool above instead. */
};

static BOOL dinput8_com_ready(void)
{
    return winecom_attach( &dinput8_surface );
}

/* ---------------------------------------------------- exported dispatch */

NTSTATUS WINAPI __wine_com_dispatch( UINT iface, UINT slot, AMD64_CONTEXT *ctx )
{
    if (!dinput8_com_ready()) return STATUS_DLL_INIT_FAILED;
    return winecom_dispatch( iface, slot, ctx );
}

/* The crossing-frequency sink's name lookup; see winecom_slot_names.  Never on
 * a dispatch path -- ntdll asks once per slot, when it interns the row. */
BOOL WINAPI __wine_com_slot_name( UINT iface, UINT slot, const char **iface_name,
                                  const char **slot_name )
{
    return winecom_slot_names( iface, slot, iface_name, slot_name );
}

/* The shared loud-refusal stub every GUEST-REFUSE export resolves to: a flat
 * export that vends or consumes interfaces and has no wrapper.  Returns
 * E_NOTIMPL, never a pass-through that would hand the guest a native vtable.
 * The trapping export's own name is in the thunk dispatcher's TRACE.  Same
 * symbol, same contract, as dlls/combase/syscom.c's -- spec2thunk's flat
 * audit resolves GUEST-REFUSE to this exact name. */
HRESULT WINAPI __wine_com_refuse(void)
{
    ERR( "dinput8: refusing an interface-bearing flat export with no wrapper "
         "(see the guest thunk trace for which)\n" );
    return E_NOTIMPL;
}

/* ------------------------------------------------------- the flat wrapper */

/* DirectInput8Create itself is declared by dinput.h, which this module
 * compiles with DIRECTINPUT_VERSION 0x0800, so there is nothing to redeclare:
 * the call below is an ordinary internal call to this module's own export. */
HRESULT WINAPI __wine_guest_DirectInput8Create( HINSTANCE hinst, DWORD version,
                                                REFIID iid, void **out,
                                                IUnknown *outer )
{
    HRESULT hr;

    if (!dinput8_com_ready())
    {
        ERR( "dinput8: the guest COM runtime did not attach; refusing "
             "DirectInput8Create rather than handing out a native vtable\n" );
        /* REFUSAL HYGIENE, BY HAND: a flat GUEST-IMPL wrapper owns its
         * out-params the way scrub_refused_outs() owns a table refusal's, and
         * an unwritten *out is stack residue the caller calls through.
         * [MEASURED] dlls/combase/syscom.c's IMMDevice::Activate is the site
         * that cost days.  The write goes through winecom_refused_scrub_ptr so
         * WINEEMUNOREFUSESCRUB can take it away and the hygiene gate can prove
         * it load-bearing.  Native failures stay untouched.  Every walker
         * refusal in this file is on an Enum* slot, which has no out-param at
         * all -- the callback and its pvRef are both IN. */
        winecom_refused_scrub_ptr( out );
        return E_FAIL;
    }
    if (outer)
    {
        /* Aggregation hands native code a GUEST IUnknown, which needs a
         * reverse proxy (design §6, step 5).  Wine's own DirectInput8Create
         * already answers DIERR_NOAGGREGATION for any non-NULL outer, so this
         * is the same answer the caller would have got anyway -- said here so
         * that the reason is legible if that ever changes. */
        FIXME( "dinput8: DirectInput8Create with a non-NULL punkOuter %p is "
               "refused for a guest until reverse proxies land\n", outer );
        winecom_refused_scrub_ptr( out );   /* refusal hygiene by hand */
        return DIERR_NOAGGREGATION;
    }
    hr = DirectInput8Create( hinst, version, iid, out, NULL );
    /* The single fail-closed choke point: a roster hit is wrapped as a proxy,
     * a miss is RELEASED and reported as E_NOINTERFACE rather than handed over
     * raw.  IID_IDirectInput8A and IID_IDirectInput8W are both in the roster,
     * so the only way to reach the miss branch is a caller asking for
     * something DirectInput8Create should not have answered. */
    return winecom_wrap_out_iface( hr, iid, out );
}
