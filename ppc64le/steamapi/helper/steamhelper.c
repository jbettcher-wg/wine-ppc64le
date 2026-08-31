/*
 * steamhelper -- the x86-64 (or i386) Linux half of the lsteamclient bridge.
 *
 * BUILT TWICE, ONE SOURCE.  This file compiles unchanged for both widths;
 * build-helper.sh --machine i386 produces steamhelper32 beside steamhelper,
 * exactly as Proton ships i386-unix/lsteamclient.so beside its x86-64 one.
 * A frame is the params struct's own bytes, so a client and a helper of
 * different pointer widths cannot speak to each other at all -- the width is
 * encoded in the frame magic (steamrpc_wire.h: 'SCR3' on i386, 'SCR1' on
 * x86-64) and the frame loop below validates it before reading any length,
 * so a mismatched pair refuses out loud instead of misreading a blob.
 *
 * WHY THIS IS A SEPARATE PROCESS.  The real Steam client library is
 * ~/.steam/sdk64/steamclient.so (sdk32 for the i386 build): a SysV ELF that
 * wants a Linux loader, a Linux libc, and threads.  Native Wine on this port is ppc64le and
 * cannot dlopen it, and the embedded emulator runs x86-64 *Windows* code, not
 * x86-64 Linux code.  So the unix half of Proton's lsteamclient -- which is
 * already a clean, flat, params-struct-in/params-struct-out interface -- is
 * compiled into this program instead, which runs under FEX via binfmt like
 * any other x86-64 Linux binary, and is spoken to over a socket.
 *
 * WHAT IS VENDORED HERE.  Proton's unix side, unmodified: unixlib.cpp,
 * unixlib_generated.cpp, the 213 cppISteam*.cpp per-interface wrappers and the
 * five unix_steam_*_manual.cpp files, all of which compile for x86-64 Linux
 * against this tree's Wine headers exactly as they do inside Wine (measured:
 * 219 files, zero errors).  So the helper executes the SAME code Proton does,
 * with the SAME struct conversions and the SAME callback bookkeeping.  This
 * file adds only the transport, the ntdll entry points a Wine unixlib gets for
 * free and a standalone program does not (steamhelper_stub.c), and the
 * DOS<->unix file name translation those bottom out in, which needs a prefix
 * this process does not have (steamhelper_path.c).
 *
 * WHAT THE HELPER KNOWS ABOUT STEAMWORKS: nothing.  Every frame carries its
 * own pointer map, built on the client side by the compiler (see
 * tools/steamrpc/gen-steamrpc).  The helper copies each blob into its own
 * memory, patches the pointer field to its copy, calls
 * __wine_unix_call_funcs[code], and sends the OUT copies back.  Adding an
 * interface version therefore needs no change here at all.
 *
 * SAFETY.  --probe only dlopens and dlsyms; it never calls CreateInterface,
 * so it cannot make this machine's Steam client think a game started.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "steamhelper.h"

static int verbose;

void steamhelper_log( const char *fmt, ... )
{
    va_list ap;

    if (!verbose) return;
    fputs( "helper: ", stderr );
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fflush( stderr );
}

/* ------------------------------------------------------------------ probe */

static const char *const probe_exports[] =
{
    "CreateInterface",
    "Steam_BGetCallback",
    "Steam_GetAPICallResult",
    "Steam_FreeLastCallback",
    "Steam_ReleaseThreadLocalMemory",
    "Steam_IsKnownInterface",
    "Steam_NotifyMissingInterface",
};

static int probe(void)
{
    char path[PATH_MAX];
    const char *env, *home;
    void *lib;
    unsigned i;
    int missing = 0;
    struct utsname u;

    uname( &u );
    printf( "helper: uname machine = %s\n", u.machine );
    printf( "helper: pointer size = %zu\n", sizeof(void *) );

    if ((env = getenv( "STEAMCLIENT_SO" ))) snprintf( path, sizeof(path), "%s", env );
    else
    {
        if (!(home = getenv( "HOME" ))) home = "";
        /* WHICH SDK, AND WHY IT IS NOT A CHOICE.  Steam ships two client
         * libraries -- ~/.steam/sdk64 and ~/.steam/sdk32 -- and a process can
         * only load the one matching its own word size.  Proton's own
         * unixlib.cpp already picks between them on __i386__ (its STEAM_ARCH),
         * so the SERVE path gets this right for free; this is the probe path,
         * which resolves the name itself in order to report it, and it has to
         * agree with what serve would actually dlopen or --probe would be
         * checking a library the helper never loads. */
#ifdef __i386__
        snprintf( path, sizeof(path), "%s/.steam/sdk32/steamclient.so", home );
#else
        snprintf( path, sizeof(path), "%s/.steam/sdk64/steamclient.so", home );
#endif
    }
    printf( "helper: steamclient = %s\n", path );

    if (!(lib = dlopen( path, RTLD_NOW )))
    {
        printf( "helper: dlopen FAILED: %s\n", dlerror() );
        return 1;
    }
    printf( "helper: dlopen ok, handle = %p\n", lib );
    for (i = 0; i < sizeof(probe_exports) / sizeof(probe_exports[0]); i++)
    {
        void *p = dlsym( lib, probe_exports[i] );
        if (p) printf( "export %s = %p\n", probe_exports[i], p );
        else { printf( "export %s = MISSING\n", probe_exports[i] ); missing++; }
    }
    return missing ? 1 : 0;
}

/* -------------------------------------------------------------- transport */

static int read_all( int fd, void *buf, size_t len )
{
    char *p = buf;

    while (len)
    {
        ssize_t n = read( fd, p, len );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static int write_all( int fd, const void *buf, size_t len )
{
    const char *p = buf;

    while (len)
    {
        ssize_t n = write( fd, p, len );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

/* ------------------------------------------------------------- selftest */

/* The other end of steamrpc.c's __wine_steamrpc_selftest.  Every value it
 * writes is one the client checks, and every value it reads is one the client
 * set, so a length or a direction that is off by anything shows up as a named
 * bit rather than as a game misbehaving three hours later.  It touches
 * nothing outside the params struct and never loads steamclient.so, so it is
 * runnable on a machine with no Steam at all. */
static NTSTATUS steamhelper_selftest( char *params, unsigned int len )
{
    struct steamrpc_selftest_params *p = (struct steamrpc_selftest_params *)params;
    unsigned int i, nbuf;

    if (len != sizeof(*p))
    {
        fprintf( stderr, "helper: selftest params are %u bytes here and %u "
                 "bytes on the client; the two ends disagree about the wire\n",
                 (unsigned)sizeof(*p), len );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }

    nbuf = p->buf_len;   /* captured before the in-string verdict overwrites it */
    p->_ret = STEAMRPC_SELFTEST_RET;
    /* buf_len doubles as the in-string verdict: zeroed means the string did
     * not arrive intact, which the client reports as BAD_STR_IN. */
    if (!p->str_in || strcmp( p->str_in, STEAMRPC_SELFTEST_IN )) p->buf_len = 0;

    if (p->str_out && p->str_out_len > sizeof(STEAMRPC_SELFTEST_OUT))
        memcpy( p->str_out, STEAMRPC_SELFTEST_OUT, sizeof(STEAMRPC_SELFTEST_OUT) );
    if (p->fixed)
    {
        p->fixed->a += 1;
        p->fixed->b += 1;
        p->fixed->c += 1;
    }
    if (p->buf)
        for (i = 0; i < nbuf; i++) p->buf[i] = (unsigned char)(i * 7 + 3);
    p->handle ^= STEAMRPC_SELFTEST_XOR;
    return 0;
}

/* ------------------------------------------------------------ the drives */

static NTSTATUS steamhelper_setdrives( char *params, unsigned int len )
{
    struct steamrpc_setdrives_params *p = (struct steamrpc_setdrives_params *)params;

    if (len != sizeof(*p))
    {
        fprintf( stderr, "helper: the drive-map params are %u bytes here and "
                 "%u on the client; the two ends disagree about the wire\n",
                 (unsigned)sizeof(*p), len );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    if (p->stride != sizeof(struct steamrpc_drive))
    {
        fprintf( stderr, "helper: a drive record is %u bytes here and %u on "
                 "the client; refusing the map rather than reading it at the "
                 "wrong stride\n", (unsigned)sizeof(struct steamrpc_drive),
                 p->stride );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    if (p->count > STEAMRPC_DRIVE_MAX || !p->drives)
    {
        fprintf( stderr, "helper: refusing a drive map of %u drives\n", p->count );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    p->_ret = steamhelper_set_drives( p->drives, p->count );
    return 0;
}

/* ------------------------------------------------------- the red zone, tested
 *
 * Writes past the end of a blob on purpose, so the guard check below has
 * something to catch and the gate can watch it happen.  The overrun is clamped
 * to the red zone this helper allocated, so the bytes written are always its
 * own slack: this proves the safety net without ever being the thing the net
 * exists to stop.
 */
static NTSTATUS steamhelper_overrun( char *params, unsigned int len )
{
    struct steamrpc_overrun_params *p = (struct steamrpc_overrun_params *)params;
    unsigned int past, i;

    if (len != sizeof(*p) || !p->buf) return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;

    past = p->past < STEAMRPC_BLOB_GUARD ? p->past : STEAMRPC_BLOB_GUARD;
    for (i = 0; i < past; i++) p->buf[p->len + i] = (unsigned char)(i + 1);
    p->_ret = past;
    return 0;
}

/* --------------------------------------------------------- synthetic callbacks
 *
 * Steamworks callbacks are pull-based, so a gate can prove delivery end to end
 * with no Steam client PROVIDED something can put one into the far end of a
 * pull channel.  That is all this is: it queues into the very structures the
 * real client's callbacks go into, and everything after that -- the wire, the
 * descriptors, Proton's PE-side Steam_BGetCallback and
 * execute_pending_callbacks, the game's own function pointer -- is the shipped
 * path, unmodified.  A synthetic delivery on THIS side would have proved
 * nothing about the side that actually breaks.
 *
 * Two channels, because a game uses both:
 *
 *   CDECL   queue_cdecl_func_callback(), Proton's own queue -- the one that
 *           carries SetWarningMessageHook, SetDebugOutputFunction and
 *           EnableActionEventCallbacks back to a guest function pointer.  The
 *           helper stores the guest address and never calls it; the PE side
 *           calls it, which is why that class is bridgeable at all.
 *   MSG     one CallbackMsg_t answered to Steam_BGetCallback -- the channel
 *           steam_api64.dll's SteamAPI_ManualDispatch_GetNextCallback and
 *           SteamAPI_RunCallbacks are both built on.  The payload travels
 *           back through the m_pubParam pointer nested inside the message,
 *           which is the one place in this whole surface where a marshalled
 *           struct holds a live client address.
 *
 * Nothing here is consulted until an explicit STEAMRPC_CODE_INJECT frame
 * arrives, and the helper listens on loopback only, so a game cannot reach it.
 * The armed state is per PROCESS rather than per connection, deliberately: it
 * costs nothing, and a helper serving two clients at once while one of them is
 * injecting is a test harness that has already gone wrong.
 */

/* Declared in the vendored unix_private.h, which is a C++ header (it uses
 * templates in other declarations) but puts these in an extern "C" block. */
typedef void (*w_cdecl_func_c)( void * );
extern void queue_cdecl_func_callback( w_cdecl_func_c func, void *data,
                                       uint32_t data_size );

static struct
{
    int      armed;                              /* a MSG is waiting */
    uint32_t id;
    uint32_t len;
    unsigned char payload[STEAMRPC_INJECT_LEN];
} injected;

static NTSTATUS steamhelper_inject( char *params, unsigned int len )
{
    struct steamrpc_inject_params *p = (struct steamrpc_inject_params *)params;

    if (len != sizeof(*p))
    {
        fprintf( stderr, "helper: the injection params are %u bytes here and "
                 "%u on the client\n", (unsigned)sizeof(*p), len );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }
    if (p->len > sizeof(injected.payload) || !p->payload)
    {
        fprintf( stderr, "helper: refusing an injection of kind %u with a "
                 "%u-byte payload\n", p->kind, p->len );
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
    }

    switch (p->kind)
    {
    case STEAMRPC_INJECT_CDECL:
        if (!p->func) return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
        queue_cdecl_func_callback( (w_cdecl_func_c)(uintptr_t)p->func,
                                   (void *)p->payload, p->len );
        p->_ret = 1;
        return 0;
    case STEAMRPC_INJECT_MSG:
        memcpy( injected.payload, p->payload, p->len );
        injected.len = p->len;
        injected.id = p->id;
        injected.armed = 1;
        p->_ret = 1;
        return 0;
    }
    fprintf( stderr, "helper: injection kind %u is not one this end knows\n",
             p->kind );
    return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;
}

/* The cookie a synthetic message hands back in place of a u_CallbackMsg_t
 * address, so that the receive step can tell one from a real client's. */
#define INJECT_COOKIE 0x5c1ec0071e5700d1ull

/* Answers the three codes a pending synthetic message stands in for, and only
 * while one is pending.  Returns 0 when the frame is not ours to serve. */
static int inject_serve( unsigned int code, char *params, unsigned int len,
                         NTSTATUS *status )
{
    if (!injected.armed) return 0;

    if (code == unix_steamclient_Steam_BGetCallback &&
        len == sizeof(struct steamclient_Steam_BGetCallback_params))
    {
        struct steamclient_Steam_BGetCallback_params *p =
            (struct steamclient_Steam_BGetCallback_params *)params;

        if (!p->w_msg) return 0;
        p->w_msg->m_hSteamUser = STEAMRPC_INJECT_USER;
        p->w_msg->m_iCallback = (int32_t)injected.id;
        p->w_msg->m_cubParam = (int32_t)injected.len;
        p->cookie = INJECT_COOKIE;
        p->_ret = 1;
        *status = 0;
        return 1;
    }

    if (code == unix_steamclient_callback_message_receive &&
        len == sizeof(struct steamclient_callback_message_receive_params))
    {
        struct steamclient_callback_message_receive_params *p =
            (struct steamclient_callback_message_receive_params *)params;
        uint32_t n;

        if (p->cookie != INJECT_COOKIE || !p->w_msg) return 0;
        n = injected.len;
        if (p->w_msg->m_cubParam >= 0 && (uint32_t)p->w_msg->m_cubParam < n)
            n = (uint32_t)p->w_msg->m_cubParam;
        if (p->w_msg->m_pubParam) memcpy( p->w_msg->m_pubParam, injected.payload, n );
        *status = 0;
        return 1;
    }

    if (code == unix_steamclient_Steam_FreeLastCallback &&
        len == sizeof(struct steamclient_Steam_FreeLastCallback_params))
    {
        struct steamclient_Steam_FreeLastCallback_params *p =
            (struct steamclient_Steam_FreeLastCallback_params *)params;

        injected.armed = 0;
        p->_ret = 1;
        *status = 0;
        return 1;
    }

    return 0;
}

/* ---------------------------------------------------------------- paths */

/* Proton's own converters, the ones the 85 path-bearing wrappers call.  They
 * live in the vendored unix_private.h, which this C file cannot include (it is
 * a C++ header); all three are extern "C" there.  api_result is a C++ `bool`
 * there and is spelled _Bool here rather than int: the two are the same
 * one-byte object in the SysV ABI, and int would be a declaration that
 * happens to work rather than one that matches. */
extern char *steamclient_dos_to_unix_path( const char *src, int is_url );
extern void steamclient_free_path( char *path );
extern unsigned int steamclient_unix_path_to_dos_path( _Bool api_result, const char *src,
                                                       char *dst, uint32_t dst_bytes,
                                                       int is_url );

static NTSTATUS steamhelper_pathtest( char *params, unsigned int len )
{
    struct steamrpc_pathtest_params *p = (struct steamrpc_pathtest_params *)params;
    char *converted;

    if (len != sizeof(*p) || !p->dos_in || !p->unix_out || !p->dos_back)
        return (NTSTATUS)STEAMRPC_STATUS_PROTOCOL;

    p->ndrives = steamhelper_drive_count();
    p->unix_out[0] = 0;
    p->dos_back[0] = 0;

    if (!(converted = steamclient_dos_to_unix_path( p->dos_in, 0 ))) return 0;
    snprintf( p->unix_out, p->unix_len, "%s", converted );
    p->_ret = steamclient_unix_path_to_dos_path( 1, converted, p->dos_back,
                                                 p->back_len, 0 );
    steamclient_free_path( converted );
    return 0;
}

/* ------------------------------------------------------------- dispatch */

/* Proton's unix side keeps a pointer to a 4096-byte scratch buffer the PE
 * side owns and writes DOS-converted paths into it.  Across a process that
 * buffer has to be ours, so steamclient_init's g_tmppath field is redirected
 * here and the contents are shipped back to the client whenever they change.
 * Anything else would leave the client holding a helper address. */
static char helper_tmppath[STEAMRPC_TMPPATH_LEN];
static char helper_tmppath_sent[STEAMRPC_TMPPATH_LEN];

/* One call at a time.  The client serialises its own calls behind a critical
 * section, and a second client would otherwise interleave with the first
 * through steamclient.so's single set of globals. */
static pthread_mutex_t call_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_BLOBS 32

struct blob
{
    uint32_t offset;
    uint32_t len;
    uint32_t flags;
    uint32_t inner;
    void    *mem;      /* len bytes, then STEAMRPC_BLOB_GUARD red-zone bytes */
};

/* How far past the end of a blob something wrote, or 0 if the red zone is
 * untouched.  See the guard comment in steamrpc_wire.h for why this exists:
 * without it, a marshal descriptor that is one element short is a corrupted
 * glibc arena and a dead helper rather than a named parameter. */
static unsigned int blob_overrun( const struct blob *b )
{
    const unsigned char *guard = (const unsigned char *)b->mem + b->len;
    unsigned int i, past = 0;

    for (i = 0; i < STEAMRPC_BLOB_GUARD; i++)
        if (guard[i] != STEAMRPC_BLOB_GUARD_BYTE) past = i + 1;
    return past;
}

static int serve_one_call( int fd )
{
    struct steamrpc_req_hdr req;
    struct steamrpc_rep_hdr rep;
    struct blob blobs[MAX_BLOBS];
    unsigned int i, nblobs = 0, nout = 0;
    char *params = NULL;
    NTSTATUS status;
    int ok = 0;

    if (!read_all( fd, &req, sizeof(req) )) return 0;   /* clean EOF */
    if (req.magic != STEAMRPC_REQ_MAGIC ||
        req.params_len > STEAMRPC_MAX_FRAME ||
        req.nblobs > MAX_BLOBS ||
        (req.code >= steamhelper_func_count && !steamrpc_is_local_code( req.code )))
    {
        fprintf( stderr, "helper: refusing a frame with magic %#x code %u "
                 "params %u blobs %u\n", req.magic, req.code, req.params_len,
                 req.nblobs );
        return 0;
    }

    if (req.params_len && !(params = calloc( 1, req.params_len ))) return 0;
    if (req.params_len && !read_all( fd, params, req.params_len )) goto done;

    for (i = 0; i < req.nblobs; i++)
    {
        struct steamrpc_blob_hdr bh;

        if (!read_all( fd, &bh, sizeof(bh) )) goto done;
        if (bh.len > STEAMRPC_MAX_FRAME ||
            (uint64_t)bh.offset + sizeof(void *) > req.params_len)
        {
            fprintf( stderr, "helper: blob at offset %u len %u does not fit a "
                     "%u-byte params struct\n", bh.offset, bh.len,
                     req.params_len );
            goto done;
        }
        /* The red zone also means a zero-length pointee still gets an
         * allocation, so the pointer the callee sees is non-NULL exactly when
         * the caller's was. */
        if (!(blobs[nblobs].mem = calloc( 1, bh.len + STEAMRPC_BLOB_GUARD ))) goto done;
        memset( (char *)blobs[nblobs].mem + bh.len, STEAMRPC_BLOB_GUARD_BYTE,
                STEAMRPC_BLOB_GUARD );
        if (bh.len && !read_all( fd, blobs[nblobs].mem, bh.len )) goto done;
        blobs[nblobs].offset = bh.offset;
        blobs[nblobs].len = bh.len;
        blobs[nblobs].flags = bh.flags;
        blobs[nblobs].inner = bh.inner;

        if (bh.flags & STEAMRPC_NESTED)
        {
            /* The pointer to patch is inside an EARLIER blob, not in the
             * params struct: the client emits the outer field first, so it is
             * already here.  Refusing when it is not is the fail-closed
             * answer -- patching a client address into helper memory is
             * exactly the bug this class exists to prevent. */
            unsigned int k;

            for (k = 0; k < nblobs; k++)
                if (blobs[k].offset == bh.offset && !(blobs[k].flags & STEAMRPC_NESTED))
                    break;
            if (k == nblobs || (uint64_t)bh.inner + sizeof(void *) > blobs[k].len)
            {
                fprintf( stderr, "helper: nested blob at offset %u+%u has no "
                         "outer blob to patch\n", bh.offset, bh.inner );
                free( blobs[nblobs].mem );
                goto done;
            }
            memcpy( (char *)blobs[k].mem + bh.inner, &blobs[nblobs].mem,
                    sizeof(void *) );
        }
        else memcpy( params + bh.offset, &blobs[nblobs].mem, sizeof(void *) );

        if (bh.flags & STEAMRPC_OUT) nout++;
        nblobs++;
    }

    pthread_mutex_lock( &call_lock );

    if (req.code == unix_steamclient_init && req.params_len)
    {
        /* Redirect the scratch buffer at ours; the blob the client sent for
         * it carries its current contents, which we keep. */
        for (i = 0; i < nblobs; i++)
            if (blobs[i].offset == offsetof(struct steamclient_init_params, g_tmppath))
            {
                memcpy( helper_tmppath, blobs[i].mem,
                        blobs[i].len < sizeof(helper_tmppath) ? blobs[i].len
                                                              : sizeof(helper_tmppath) );
                break;
            }
        ((struct steamclient_init_params *)params)->g_tmppath = helper_tmppath;
    }

    /* inject_serve() stands in for the three codes a pending synthetic
     * callback covers, and answers 0 for everything else -- including those
     * three when nothing has been injected, which is every run of a real
     * game.  It is asked before the real dispatch and after the local codes,
     * so an injection frame can never be shadowed by it. */
    if (req.code == STEAMRPC_CODE_SELFTEST)
        status = steamhelper_selftest( params, req.params_len );
    else if (req.code == STEAMRPC_CODE_SETDRIVES)
        status = steamhelper_setdrives( params, req.params_len );
    else if (req.code == STEAMRPC_CODE_INJECT)
        status = steamhelper_inject( params, req.params_len );
    else if (req.code == STEAMRPC_CODE_PATHTEST)
        status = steamhelper_pathtest( params, req.params_len );
    else if (req.code == STEAMRPC_CODE_OVERRUN)
        status = steamhelper_overrun( params, req.params_len );
    else if (!inject_serve( req.code, params, req.params_len, &status ))
        status = steamhelper_call( req.code, req.params_len ? params : NULL );

    memset( &rep, 0, sizeof(rep) );
    rep.magic = STEAMRPC_REP_MAGIC;
    rep.status = (uint32_t)status;
    rep.params_len = req.params_len;
    rep.nblobs = nout;
    rep.tmppath_base = (uint64_t)(uintptr_t)helper_tmppath;

    /* The red zone, checked while the call's own memory is still the only
     * thing that could have been damaged.  A blown guard means the marshal
     * descriptor for that parameter is smaller than what the Steam client
     * actually writes -- the class of mistake that used to end as
     * `free(): invalid size` in this process and a dead bridge in the game's.
     * The call is refused rather than answered, because the parameter's
     * contents are now as untrustworthy as its length, and the reply carries
     * the params offset so that the client -- which is the end that holds the
     * method and parameter NAMES -- can say which one it was. */
    for (i = 0; i < nblobs; i++)
    {
        unsigned int past = blob_overrun( &blobs[i] );

        if (!past) continue;
        fprintf( stderr, "helper: unix call %u wrote %u byte%s past the end of "
                 "the %u-byte buffer marshalled for the parameter at params "
                 "offset %u.  The red zone absorbed it, so this process is "
                 "intact; the call is refused because that parameter's "
                 "marshal descriptor is too small.\n",
                 req.code, past, past == 1 ? "" : "s", blobs[i].len,
                 blobs[i].offset );
        rep.status = STEAMRPC_STATUS_OVERFLOW;
        rep.detail = blobs[i].offset;
        rep.nblobs = nout = 0;   /* answer nothing rather than half of it */
        break;
    }
    if (memcmp( helper_tmppath, helper_tmppath_sent, sizeof(helper_tmppath) ))
    {
        memcpy( helper_tmppath_sent, helper_tmppath, sizeof(helper_tmppath) );
        rep.tmppath_len = sizeof(helper_tmppath);
    }

    pthread_mutex_unlock( &call_lock );

    if (!write_all( fd, &rep, sizeof(rep) )) goto done;
    if (req.params_len && !write_all( fd, params, req.params_len )) goto done;
    for (i = 0; i < nblobs && rep.nblobs; i++)
    {
        struct steamrpc_blob_hdr bh;

        if (!(blobs[i].flags & STEAMRPC_OUT)) continue;
        bh.offset = blobs[i].offset;
        bh.len = blobs[i].len;
        bh.flags = STEAMRPC_OUT | (blobs[i].flags & STEAMRPC_NESTED);
        bh.inner = blobs[i].inner;
        if (!write_all( fd, &bh, sizeof(bh) )) goto done;
        if (bh.len && !write_all( fd, blobs[i].mem, bh.len )) goto done;
    }
    if (rep.tmppath_len && !write_all( fd, helper_tmppath_sent, rep.tmppath_len ))
        goto done;

    ok = 1;

done:
    for (i = 0; i < nblobs; i++) free( blobs[i].mem );
    free( params );
    return ok;
}

/* --exit-when-idle: leave once the process that was launched to talk to us
 * has gone.  See its comment in serve(). */
static int exit_when_idle;
static int live_connections;
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

static void *serve_connection( void *arg )
{
    int fd = (int)(intptr_t)arg;

    steamhelper_log( "accepted connection on fd %d\n", fd );
    while (serve_one_call( fd )) /* nothing */;
    steamhelper_log( "connection on fd %d finished\n", fd );
    close( fd );

    pthread_mutex_lock( &conn_lock );
    live_connections--;
    if (exit_when_idle && !live_connections)
    {
        steamhelper_log( "the last client is gone; exiting\n" );
        pthread_mutex_unlock( &conn_lock );
        /* _exit rather than exit: another thread may still be inside
         * steamclient.so, and running its atexit handlers underneath one is a
         * worse way to leave than not running them at all. */
        _exit( 0 );
    }
    pthread_mutex_unlock( &conn_lock );
    return NULL;
}

static int serve( int want_port )
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int lfd, one = 1;

    signal( SIGPIPE, SIG_IGN );

    /* DO NOT OUTLIVE WHOEVER STARTED US.  The compat tool starts this helper
     * in the background and kills it from an EXIT trap -- but a trap does not
     * run when the tool itself is killed with SIGKILL, which is how a stuck
     * run gets cleaned up, and a leaked helper then sits holding a port and a
     * connection to the user's Steam client forever.  MEASURED: one was found
     * still running hours after the run that started it.
     *
     * The kernel can do this without anyone's cooperation, so it does.  Set
     * before the socket exists, so there is no window where a leak is
     * possible. */
    if (prctl( PR_SET_PDEATHSIG, SIGTERM ) < 0)
        perror( "helper: PR_SET_PDEATHSIG (this helper may outlive its parent)" );
    /* ...and if the parent was already gone when we got here, PDEATHSIG will
     * never fire, so check once. */
    if (getppid() == 1)
    {
        fprintf( stderr, "helper: started with no live parent; refusing to run "
                 "as an orphan holding a Steam connection\n" );
        return 2;
    }

    if ((lfd = socket( AF_INET, SOCK_STREAM, 0 )) < 0)
    {
        perror( "helper: socket" );
        return 2;
    }
    setsockopt( lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one) );
    memset( &sin, 0, sizeof(sin) );
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl( INADDR_LOOPBACK );   /* loopback only */
    sin.sin_port = htons( (uint16_t)want_port );
    if (bind( lfd, (struct sockaddr *)&sin, sizeof(sin) ) ||
        listen( lfd, 8 ) ||
        getsockname( lfd, (struct sockaddr *)&sin, &len ))
    {
        perror( "helper: bind/listen" );
        close( lfd );
        return 2;
    }

    /* The port is the one thing the client cannot guess, and a port 0 request
     * is the normal case, so it goes to stdout on its own line and stdout is
     * flushed: whoever started us reads this line and puts it in the game's
     * STEAM_BRIDGE_ADDR. */
    printf( "helper: listening on 127.0.0.1:%u\n", ntohs( sin.sin_port ) );
    fflush( stdout );

    for (;;)
    {
        pthread_t th;
        int fd = accept( lfd, NULL, NULL );

        if (fd < 0)
        {
            if (errno == EINTR) continue;
            perror( "helper: accept" );
            break;
        }
        setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one) );
        pthread_mutex_lock( &conn_lock );
        live_connections++;
        pthread_mutex_unlock( &conn_lock );
        if (pthread_create( &th, NULL, serve_connection, (void *)(intptr_t)fd ))
        {
            fprintf( stderr, "helper: cannot start a thread for a client\n" );
            pthread_mutex_lock( &conn_lock );
            live_connections--;
            pthread_mutex_unlock( &conn_lock );
            close( fd );
            continue;
        }
        pthread_detach( th );
    }
    close( lfd );
    return 0;
}

/* ----------------------------------------------------------------- main */

static void usage(void)
{
    fprintf( stderr,
             "usage: steamhelper --probe\n"
             "       steamhelper --serve [--port N] [--exit-when-idle] [-v]\n"
             "\n"
             "--probe  dlopen the native steamclient.so and report its exports.\n"
             "         Never calls into it, so it cannot disturb a running client.\n"
             "--serve  listen on 127.0.0.1 for the guest steamclient64.dll\n"
             "         (steamclient.dll for an i386 build of this helper).\n"
             "         --port 0 (the default) picks a free port and prints it.\n"
             "--exit-when-idle\n"
             "         leave once the last client disconnects.  For a single\n"
             "         game launch, where one process connects once; NOT for\n"
             "         the gate, which drives several probes past one helper.\n" );
}

int main( int argc, char **argv )
{
    int i, port = 0, mode = 0;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp( argv[i], "--probe" )) mode = 1;
        else if (!strcmp( argv[i], "--serve" )) mode = 2;
        else if (!strcmp( argv[i], "-v" )) verbose = 1;
        else if (!strcmp( argv[i], "--exit-when-idle" )) exit_when_idle = 1;
        else if (!strcmp( argv[i], "--port" ) && i + 1 < argc) port = atoi( argv[++i] );
        else { usage(); return 2; }
    }
    if (mode == 1) return probe();
    if (mode == 2) return serve( port );
    usage();
    return 2;
}
