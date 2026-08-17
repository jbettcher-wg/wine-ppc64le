/*
 * steamhelper.h -- what the helper's transport needs from the vendored
 * Proton unix side.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __STEAMHELPER_H
#define __STEAMHELPER_H

#include <stdarg.h>
#include <limits.h>

#include "windef.h"
#include "winbase.h"

/* Proton's params structs and its enum unix_funcs, byte-identical to the ones
 * the guest steamclient64.dll compiles: both ends are x86-64 and both take
 * them from this same header. */
#include "steamclient_structs.h"
#include "unixlib.h"

#include "steamrpc_wire.h"
#include "steamrpc_count.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* steamhelper_stub.c */
extern NTSTATUS steamhelper_call( unsigned int code, void *params );
extern const unsigned int steamhelper_func_count;

/* steamhelper.c */
extern void steamhelper_log( const char *fmt, ... );

/* steamhelper_path.c */
extern unsigned int steamhelper_set_drives( const struct steamrpc_drive *in,
                                            unsigned int count );
extern unsigned int steamhelper_drive_count( void );

#ifdef __cplusplus
}
#endif

#endif /* __STEAMHELPER_H */
