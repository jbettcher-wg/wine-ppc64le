/*
 * emu_xstat -- guest/native crossing counters for the ppc64le native lane.
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

#ifndef __WINE_NTDLL_EMU_XSTAT_H
#define __WINE_NTDLL_EMU_XSTAT_H

/* The row array lives in one allocation the unix side owns and both sides
 * address directly; the PE side counts into it and the unix side is the only
 * one that formats or writes, because only it has stdio and the sink path. */

#define EMU_XSTAT_FLAT      0   /* spec2thunk flat export, per (module, export) */
#define EMU_XSTAT_COM       1   /* winecom vtable slot, per (interface, slot) */
#define EMU_XSTAT_SYSCALL   2   /* __wine_syscall_dispatcher, per syscall */
#define EMU_XSTAT_CALLBACK  3   /* native -> guest trampoline, per target */
#define EMU_XSTAT_EVENT     4   /* the rest: faults, nested runs, refusals */
#define EMU_XSTAT_CLASSES   5

#define EMU_XSTAT_NAME      104

/* The EVENT class has one row per key and no identity to resolve. */
#define XSTAT_EV_TRAP_UNHANDLED  1
#define XSTAT_EV_GUEST_FAULT     2
#define XSTAT_EV_NESTED_RUN      3

/* Power of two: the intern probe masks with it.  8192 rows against a corpus
 * whose largest observed crossing surface is a few hundred sites leaves the
 * table under 5% loaded, so the open-addressed probe stays at one step. */
#define EMU_XSTAT_ROWS      8192

struct emu_xstat_row
{
    ULONG64   count;                 /* relaxed atomic adds from any thread */
    ULONG_PTR key;                   /* identity within cls; 0 = free */
    UINT      cls;
    UINT      pad;
    char      name[EMU_XSTAT_NAME];
};

struct emu_xstat_ctl
{
    struct emu_xstat_row *rows;
    UINT          rows_max;          /* EMU_XSTAT_ROWS, or 0 when off */
    volatile LONG dump_req;          /* SIGUSR2 raises it; the PE side clears */
};

/* unix_emu_xstat_init */
struct emu_xstat_init_params
{
    struct emu_xstat_ctl *ctl;       /* out: NULL when the sink is not armed */
};

#endif /* __WINE_NTDLL_EMU_XSTAT_H */
