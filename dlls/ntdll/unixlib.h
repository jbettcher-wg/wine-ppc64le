/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
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

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

/* Runs an x86-64 guest entry point through the embedded emulator and returns
 * when the guest returns to its caller.  ppc64le-only: the native machine
 * cannot execute the main image's code, and the WoW64 machinery is 32-on-64
 * by construction, so a 64-bit guest image is dispatched here instead —
 * from RtlUserThreadStart, the last common chokepoint before the first guest
 * instruction would run.  See dlls/ntdll/signal_ppc64.c. */
struct emu_run_entry_params
{
    void      *entry;    /* guest entry point (MS-x64: arg in RCX) */
    void      *arg;      /* single argument (PEB for the main thread) */
    ULONGLONG  retval;   /* out: guest RAX when the entry returned */
    /* PE-side entry for a guest trap.  Resolving the call needs the module
     * list and the export tables, which only the PE loader has, and the
     * functions it dispatches to are Win32 code -- so it is entered on the
     * Win32 stack through call_user_mode_callback() and returns through
     * NtCallbackReturn, with the same (id, args, len) shape as
     * KiUserCallbackDispatcher.  args holds one pointer: the AMD64 CONTEXT the
     * emulator marshalled, whose Rip the dispatcher owns.  Returning
     * STATUS_SUCCESS resumes the guest, anything else ends the run.  See
     * emu_trap_dispatch() in dlls/ntdll/signal_ppc64.c. */
    void     (*trap_dispatcher)( ULONG id, void *args, ULONG len );
};

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
    unix_emu_run_entry,
};

extern unixlib_handle_t __wine_unixlib_handle;

#endif /* __NTDLL_UNIXLIB_H */
