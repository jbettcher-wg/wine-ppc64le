/*
 * flat_lever_smoke.c -- the guest observable for the WINEEMUNOFLAT* levers.
 *
 * WHAT IT MEASURES, and why it is GetProcAddress rather than a call.  The
 * state these levers restore is "spec2thunk never emitted this export": the
 * guest thunk PE has no such export, so an IMPORT of it binds to ntdll's
 * per-symbol 0xdead0000+n sentinel and a GetProcAddress of it answers NULL.
 * A probe that IMPORTED the export under test would therefore have to CALL an
 * unmapped address to observe the lever -- it would die, correctly, and prove
 * the point by crashing, which is no way to run a gate.  GetProcAddress reads
 * the same resolution through the same find_ordinal_export() and answers with
 * a value instead of a fault.  Both routes are the pre-tier behaviour; this is
 * the half that can be printed.
 *
 * The names come from ppc64le/thunks/flat_lever_names.h, which the gate
 * generates by reading the SIGNATURE TIER out of the BUILT guest DLLs
 * (ppc64le/thunks/flat-tier-rows.py).  Nothing is hard-coded here: an export
 * list typed into a probe drifts from the artifact silently, and a leg that
 * probes an export the tier no longer serves reports "forced" for a row that
 * was never served in the first place.
 *
 * No CRT, no imports beyond the four kernel32 entry points the gate's .def
 * names -- the same rule as ppc64le/winecom/probes/com_lever_smoke.c, so the
 * probe binds to what a real guest binds to and nothing else is linked in.
 */

/* windows.h whole, exactly as com_lever_smoke.c does it: winbase.h declares
 * FormatMessage with a __ms_va_list*, so it needs stdarg.h to have been
 * pulled in first, and windows.h is what does that in the right order. */
#include <windows.h>

static HANDLE out;

static void put( const char *s )
{
    DWORD n = 0, len = 0;

    while (s[len]) len++;
    WriteFile( out, s, len, &n, NULL );
}

/* The probe table.  FLAT_PROBE( tag, module, export ) per line; the gate
 * writes it from the built DLLs' own descriptor rows. */
#define FLAT_PROBE(tag, mod, fn) { tag, mod, fn },
static const struct { const char *tag, *mod, *fn; } probes[] =
{
#include "flat_lever_names.h"
};
#undef FLAT_PROBE

void __stdcall flat_lever_entry(void)
{
    unsigned int i;

    out = GetStdHandle( STD_OUTPUT_HANDLE );
    put( "flat_lever_smoke: start\n" );

    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
    {
        /* LoadLibraryA per probe rather than once: the guest loader answers
         * from its own namespace and a module that is already loaded costs
         * nothing here, while hoisting it would make one module's failure
         * silently change another probe's answer. */
        HMODULE mod = LoadLibraryA( probes[i].mod );
        const void *proc = mod ? (const void *)GetProcAddress( mod, probes[i].fn ) : NULL;

        put( probes[i].tag );
        /* Three values, three worlds: `served` the export resolves (the tier
         * is live), `null` it does not (the pre-tier, not-emitted state a
         * lever restores), `nomodule` the module itself never loaded, which
         * is a broken leg rather than a measurement. */
        put( !mod ? "=nomodule\n" : proc ? "=served\n" : "=null\n" );
    }

    put( "flat_lever_smoke: done\n" );
    ExitProcess( 0 );
}
