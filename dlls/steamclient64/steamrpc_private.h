/*
 * steamrpc_private.h -- the client half of the lsteamclient bridge: the
 * marshal-descriptor shapes tools/steamrpc/gen-steamrpc emits, and the entry
 * point Proton's PE-side code reaches through WINE_UNIX_CALL.
 *
 * See steamrpc_wire.h for why the boundary is a socket, and steamrpc.c for
 * how a descriptor becomes a frame.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __STEAMRPC_PRIVATE_H
#define __STEAMRPC_PRIVATE_H

#include "steamclient_private.h"
#include "steamrpc_wire.h"
#include "steamrpc_count.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* How to size the pointee of one params field. */
enum steamrpc_kind
{
    STEAMRPC_K_FIXED,   /* exactly `elem` bytes -- a sizeof from the compiler */
    STEAMRPC_K_STR,     /* NUL-terminated; the string carries its own length */
    STEAMRPC_K_SIZED,   /* `elem` bytes times a count read from `lenref` */
    STEAMRPC_K_EXPR,    /* length from the method's own lenfn (hand-written) */
    STEAMRPC_K_NESTED,  /* a pointer inside the pointee of an earlier field:
                           `offset` names that field, `elem` is the pointer's
                           offset within the pointee, `lenref` the length's */
};

struct steamrpc_field
{
    const char  *name;      /* the SDK's own parameter name, for refusals */
    unsigned int offset;    /* byte offset of the pointer field in the params */
    unsigned int flags;     /* STEAMRPC_IN / STEAMRPC_OUT */
    unsigned int kind;      /* enum steamrpc_kind */
    unsigned int elem;      /* K_FIXED: total bytes.  K_SIZED: element size */
    unsigned int lenref;    /* K_SIZED: offset | (width << 16) of the count */
};

struct steamrpc_method
{
    const char  *name;
    unsigned int params_size;
    unsigned int nfields;
    const struct steamrpc_field *fields;
    /* Non-NULL means this method has no marshal plan: it is refused by name
     * with this reason rather than sent with a guessed length. */
    const char  *refuse;
    unsigned int (*lenfn)( const void *params, unsigned int idx );
};

extern const struct steamrpc_method steamrpc_methods[];
extern const unsigned int steamrpc_method_count;

/* Proton's PE side calls this through the WINE_UNIX_CALL macro; see
 * steamclient_guest.h. */
extern NTSTATUS steamrpc_call( unsigned int code, void *params );

#endif /* __STEAMRPC_PRIVATE_H */
