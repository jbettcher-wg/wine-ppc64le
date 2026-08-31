/*
 * steamrpc_wire.h -- the frame format spoken between the guest-side
 * steamclient64.dll and the x86-64 Linux helper.
 *
 * WHY THERE IS A WIRE AT ALL.  Proton's lsteamclient already flattens every
 * Steamworks method into a params struct handed across a PE<->unix call.  Both
 * halves share an address space there, so a pointer inside a params struct
 * needs no marshalling.  On this port the unix half cannot live in the
 * process: the real steamclient.so is an x86-64 SysV ELF that needs a Linux
 * loader and a Linux libc, and native Wine here is ppc64le.  So the unix half
 * runs as its own x86-64 Linux process under FEX and this file is the
 * boundary.
 *
 * WHAT THE HELPER HAS TO KNOW.  Nothing per-method.  The client sends the
 * params blob plus one descriptor per pointer field -- (offset, length,
 * direction) -- and the helper copies each blob into its own memory, patches
 * the pointer field to point at its copy, calls Proton's own unix entry point
 * for that code, and sends back the params blob and the OUT copies.  Every
 * length in the frame came from the compiler on the client side (a sizeof or
 * a caller-supplied count), which is why the helper never has to parse a
 * Steamworks signature.
 *
 * BOTH ENDS HAVE THE SAME POINTER WIDTH and both compile the same pack(1)
 * params structs, so the blob is byte-identical on the wire with no
 * conversion.  The port's native ppc64le side never looks inside a frame; it
 * only carries bytes.  Same width is a real precondition rather than an
 * observation -- see the magic below, which encodes it so that a mismatched
 * pair refuses instead of misreading.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __STEAMRPC_WIRE_H
#define __STEAMRPC_WIRE_H

#include <stdint.h>

/* THE MAGIC CARRIES THE POINTER WIDTH, and it has to.
 *
 * A frame is the params struct's own bytes: "both ends compile the same
 * pack(1) structs" above is only true while both ends have the same pointer
 * size, because every pointer field in a params struct moves every field
 * after it.  A 32-bit guest DLL therefore cannot be served by the x86-64
 * helper -- it needs an i386 one, built from the same vendored unix half
 * against Steam's own 32-bit steamclient.so, exactly as Proton ships
 * i386-unix/lsteamclient.so beside its 64-bit build.
 *
 * Until that helper exists, the failure mode that matters is the SILENT one:
 * a 32-bit client connecting to a 64-bit helper would hand it a params blob
 * it would read with the wrong offsets and answer with confident garbage.
 * So the width is in the magic.  The helper already validates the magic
 * before it reads a single length (steamhelper.c's frame loop), so a
 * mismatched pair refuses the connection and says so, with no change needed
 * on the helper side and no possibility of a wrong answer.  When the i386
 * helper is built, it compiles this header for i386 and gets the matching
 * value for free.
 */
#ifdef __i386__
#define STEAMRPC_REQ_MAGIC  0x33524353u   /* 'S','C','R','3' -- 32-bit lane */
#define STEAMRPC_REP_MAGIC  0x33504353u   /* 'S','C','P','3' */
#else
#define STEAMRPC_REQ_MAGIC  0x31524353u   /* 'S','C','R','1' */
#define STEAMRPC_REP_MAGIC  0x31504353u   /* 'S','C','P','1' */
#endif
#define STEAMRPC_HELLO      0x304c4853u   /* 'S','H','L','0' */

/* direction of a marshalled pointer field */
#define STEAMRPC_IN   0x1
#define STEAMRPC_OUT  0x2

/* A pointer that lives INSIDE another marshalled blob rather than in the
 * params struct.  Exactly one call needs it and it is worth naming: Proton's
 * steamclient_callback_message_receive is handed a w_CallbackMsg_t whose
 * m_pubParam member points at a buffer the PE side just allocated, and the
 * unix side writes the callback payload through it.  Copying only the outer
 * struct would leave the helper holding a client address and writing to it.
 * The blob's `inner` field is the byte offset of the pointer WITHIN the blob
 * named by `offset`, which the helper has already copied, so the two arrive
 * in dependency order simply by being emitted in descriptor order. */
#define STEAMRPC_NESTED 0x4

/* The unix side keeps a pointer to a 4096-byte scratch buffer on the caller's
 * side (Proton's g_tmppath) and writes DOS-converted paths into it.  Across a
 * process that buffer is the helper's, so every reply carries it back and the
 * client rebases any params pointer that landed inside it.  Constant here so
 * both ends agree without including Proton's headers. */
#define STEAMRPC_TMPPATH_LEN 4096

/* A refused or undeliverable call gets one of these rather than a wrong
 * answer.  They are deliberately outside the NTSTATUS success range so
 * Proton's own `if (status)` checks treat them as failures. */
#define STEAMRPC_STATUS_NO_HELPER   0xe0005c01u  /* nothing listening */
#define STEAMRPC_STATUS_REFUSED     0xe0005c02u  /* no marshal plan */
#define STEAMRPC_STATUS_PROTOCOL    0xe0005c03u  /* frame did not parse */
#define STEAMRPC_STATUS_TOO_BIG     0xe0005c04u  /* a length exceeded the cap */
#define STEAMRPC_STATUS_OVERFLOW    0xe0005c05u  /* a descriptor was too small */

/* Bytes of red zone the helper puts after every blob it allocates, and the
 * byte it fills them with.
 *
 * WHY THERE IS A RED ZONE AT ALL.  Every blob length in a frame comes from a
 * marshal descriptor, and a descriptor that is too small does not produce a
 * wrong answer -- it produces the Steam client writing past the end of a
 * malloc'd buffer inside the helper.  MEASURED: ISteamInput::GetConnected-
 * Controllers was described as one 8-byte handle, the client wrote sixteen,
 * and the 120 bytes of overrun landed on glibc's next chunk header; the
 * helper died of `free(): invalid size` at DOOM's title screen and took the
 * game's Steam connection with it.  The descriptor is fixed (see
 * tools/steamrpc/gen-steamrpc, which now reads Valve's own array
 * annotations), but the CLASS of mistake has to stop being fatal: a marshal
 * plan is generated code and generated code will be wrong again.
 *
 * With the red zone the overrun lands in the helper's own slack, the guard
 * check names the parameter, the call is refused, and both processes stay up.
 * 256 bytes covers every fixed-capacity array the SDK declares (the largest
 * is 16 x 8 = 128) with room to see that it was exceeded. */
#define STEAMRPC_BLOB_GUARD      256
#define STEAMRPC_BLOB_GUARD_BYTE 0x5c

/* One frame may not exceed this.  A caller buffer bigger than this is refused
 * by name instead of being truncated, because a truncated Steamworks buffer is
 * a silent wrong answer.  16 MiB covers GetImageRGBA of anything Steam vends. */
#define STEAMRPC_MAX_FRAME  (16u * 1024 * 1024)

#pragma pack(push, 1)

struct steamrpc_req_hdr
{
    uint32_t magic;        /* STEAMRPC_REQ_MAGIC */
    uint32_t code;         /* Proton's enum unix_funcs value */
    uint32_t params_len;   /* bytes of params struct following this header */
    uint32_t nblobs;       /* pointer fields described after the params */
};

struct steamrpc_blob_hdr
{
    uint32_t offset;       /* byte offset of the pointer field in the params */
    uint32_t len;          /* bytes of pointee following this header */
    uint32_t flags;        /* STEAMRPC_IN / STEAMRPC_OUT / STEAMRPC_NESTED */
    uint32_t inner;        /* STEAMRPC_NESTED: offset within the blob at
                              `offset`; otherwise unused */
};

struct steamrpc_rep_hdr
{
    uint32_t magic;        /* STEAMRPC_REP_MAGIC */
    uint32_t status;       /* NTSTATUS from Proton's unix entry, or ours */
    uint32_t params_len;
    uint32_t nblobs;       /* OUT blobs following the params */
    uint64_t tmppath_base; /* helper address of its g_tmppath, for rebasing */
    uint32_t tmppath_len;  /* bytes of g_tmppath image trailing the blobs, or
                              0 when the call did not touch it */
    uint32_t detail;       /* STEAMRPC_STATUS_OVERFLOW: the params offset of
                              the parameter whose red zone was written into,
                              so the client can name it.  Otherwise unused. */
};

/* ------------------------------------------------------------ local codes
 *
 * Five frames that are this port's own rather than Proton's: they carry no
 * Steamworks method and the helper answers them itself.  They sit at the TOP
 * of the 32-bit range rather than being appended to Proton's enum, so that
 * re-vendoring lsteamclient and regenerating that enum can never collide with
 * one.  Everything below STEAMRPC_CODE_FIRST_LOCAL is an index into
 * __wine_unix_call_funcs[]; everything at or above it is one of these.
 */
#define STEAMRPC_CODE_FIRST_LOCAL 0xfffffff0u

#define STEAMRPC_CODE_SELFTEST    0xfffffff0u  /* the marshaller self-test */
#define STEAMRPC_CODE_SETDRIVES   0xfffffff1u  /* the prefix's drive map */
#define STEAMRPC_CODE_INJECT      0xfffffff2u  /* a synthetic callback */
#define STEAMRPC_CODE_PATHTEST    0xfffffff3u  /* one path, both directions */
#define STEAMRPC_CODE_OVERRUN     0xfffffff4u  /* the red zone, on purpose */

static inline int steamrpc_is_local_code( uint32_t code )
{
    return code >= STEAMRPC_CODE_FIRST_LOCAL;
}

/* ---------------------------------------------------------------- selftest
 *
 * One synthetic call that exercises every parameter class the marshaller
 * knows, with values both ends check.  It exists because the interesting
 * failure mode of this bridge -- a length or a direction that is subtly wrong
 * -- is invisible in a "did it connect" test, and because the alternative
 * (checking it against the real Steam client) is only available on a machine
 * where Steam happens to be running.  This one runs anywhere the helper does.
 */

#define STEAMRPC_SELFTEST_RET   0x5c1e5701u
#define STEAMRPC_SELFTEST_IN    "in:hello"
#define STEAMRPC_SELFTEST_OUT   "out:world"
#define STEAMRPC_SELFTEST_XOR   0xa5a5a5a5a5a5a5a5ull

struct steamrpc_selftest_fixed
{
    uint32_t a;
    uint64_t b;
    uint16_t c;
};

struct steamrpc_selftest_params
{
    uint32_t _ret;                          /* helper sets STEAMRPC_SELFTEST_RET */
    uint32_t buf_len;                       /* caller-buffer length, in bytes */
    uint64_t handle;                        /* opaque token, helper XORs it */
    const char *str_in;                     /* in-string, helper compares */
    char *str_out;                          /* caller buffer, helper fills */
    uint32_t str_out_len;
    uint32_t pad;
    struct steamrpc_selftest_fixed *fixed;  /* fixed struct, helper increments */
    unsigned char *buf;                     /* caller buffer, helper patterns */
};

/* Bits the client sets when a check fails, so one integer names every
 * disagreement rather than the first one. */
#define STEAMRPC_SELFTEST_BAD_STATUS   0x01
#define STEAMRPC_SELFTEST_BAD_RET      0x02
#define STEAMRPC_SELFTEST_BAD_STR_IN   0x04
#define STEAMRPC_SELFTEST_BAD_STR_OUT  0x08
#define STEAMRPC_SELFTEST_BAD_FIXED    0x10
#define STEAMRPC_SELFTEST_BAD_BUF      0x20
#define STEAMRPC_SELFTEST_BAD_HANDLE   0x40
#define STEAMRPC_SELFTEST_BAD_INPLACE  0x80

/* --------------------------------------------------------- the drive map
 *
 * WHY THE CLIENT SENDS ITS DRIVES.  Proton's unix half converts file names in
 * both directions -- a game hands ISteamRemoteStorage / ISteamUGC a DOS path
 * and gets a DOS path back from ISteamApps::GetAppInstallDir -- and it does
 * that by calling ntdll_get_unix_file_name / ntdll_get_dos_file_name, which
 * inside Wine read the process's own prefix.  The helper is a separate Linux
 * process: it has no prefix, no registry and no drive mapping, so those two
 * entry points used to be refused by name and every path came back
 * unconverted.
 *
 * THE MAPPING IS THE CLIENT'S TO KNOW, THE JOIN IS THE HELPER'S TO DO.  The
 * game's process is the only party that knows WHICH prefix these paths belong
 * to, and it can ask Wine itself rather than guess: kernel32's
 * wine_get_unix_file_name resolves a drive root through the prefix's
 * dosdevices links exactly as an ordinary file open would.  So the client
 * measures the map once per connection and sends it, and the helper -- which
 * shares the filesystem, and is the side that actually sits in the middle of
 * Proton's conversion calls -- does the string work.  Neither end guesses:
 * the client cannot know which path a converter deep inside a Steamworks
 * wrapper is about to touch, and the helper cannot know where the prefix is.
 *
 * Sending it from the client also means a helper serving two prefixes cannot
 * mix them up: the map arrives on the connection it applies to.
 */
#define STEAMRPC_DRIVE_ROOT_LEN  1023
#define STEAMRPC_DRIVE_MAX       26

struct steamrpc_drive
{
    char letter;                          /* 'A'..'Z', uppercase */
    char root[STEAMRPC_DRIVE_ROOT_LEN];   /* NUL-terminated unix path */
};

struct steamrpc_setdrives_params
{
    uint32_t _ret;                  /* helper: drives it kept */
    uint32_t count;                 /* drives following */
    uint32_t stride;                /* sizeof(struct steamrpc_drive); both
                                       ends compare it before reading */
    uint32_t pad;
    struct steamrpc_drive *drives;  /* IN, `count` records */
};

/* ------------------------------------------------------ synthetic callbacks
 *
 * Steamworks callbacks are PULL-based: nothing is delivered until the game
 * asks, either through Steam_BGetCallback (which is also what
 * SteamAPI_ManualDispatch_GetNextCallback is built on) or through the queue
 * Proton's unix side fills for hooks the game registered a function pointer
 * for.  Both channels are therefore testable without a Steam client, PROVIDED
 * something can put a callback into the helper's end of them -- which is what
 * this frame does.  It exists for the same reason the selftest above does: the
 * failure mode that matters here (a payload buffer that arrives holding a
 * helper address instead of the game's) is invisible in a "did it connect"
 * test and fatal in a game, and a live client is not available on every
 * machine this gate has to be green on.
 *
 * The helper only ever looks at its injected state after an explicit frame of
 * this kind has armed it, and it listens on loopback only, so a game can
 * never reach this path by accident.
 */
#define STEAMRPC_INJECT_CDECL   1   /* queue a CALL_CDECL_FUNC_DATA callback */
#define STEAMRPC_INJECT_MSG     2   /* arm one Steam_BGetCallback message */

#define STEAMRPC_INJECT_LEN     64          /* payload bytes, both kinds */
#define STEAMRPC_INJECT_ID      0x5c1e      /* m_iCallback: no SDK callback
                                               has this id, and it is not
                                               0x14b, which Proton's PE side
                                               treats as the overlay */
#define STEAMRPC_INJECT_USER    0x5c05      /* m_hSteamUser */
#define STEAMRPC_INJECT_BYTE(i) ((unsigned char)((i) * 13 + 7))

struct steamrpc_inject_params
{
    uint32_t _ret;                 /* helper: 1 = queued or armed */
    uint32_t kind;                 /* STEAMRPC_INJECT_* */
    uint64_t func;                 /* guest cdecl function, _CDECL only */
    uint32_t id;                   /* m_iCallback, _MSG only */
    uint32_t len;                  /* payload bytes */
    const unsigned char *payload;  /* IN, `len` bytes */
};

/* ----------------------------------------------------- the path unit test
 *
 * One DOS path, converted to unix and back, through the very functions the
 * 85 vendored wrappers call (steamclient_dos_to_unix_path and
 * steamclient_unix_path_to_dos_path).  Nothing about it is Steam-specific,
 * which is the point: it can be checked against a path the gate can compute
 * for itself, with no client and no cloud save.
 */
#define STEAMRPC_PATHTEST_LEN 1024

struct steamrpc_pathtest_params
{
    uint32_t _ret;         /* bytes unix_path_to_dos_path reported */
    uint32_t ndrives;      /* drives the helper is holding */
    const char *dos_in;    /* IN string */
    char *unix_out;        /* OUT, unix_len bytes */
    uint32_t unix_len;
    uint32_t back_len;
    char *dos_back;        /* OUT, back_len bytes */
};

/* ------------------------------------------------------- the red zone, tested
 *
 * A safety net nobody has ever seen catch anything is not a safety net.  This
 * frame asks the helper to write a stated number of bytes PAST the end of a
 * buffer whose length the client declared -- exactly what
 * ISteamInput::GetConnectedControllers did by accident -- so that the gate can
 * watch the guard catch it, the call get refused by name, and both processes
 * carry on.
 *
 * It cannot itself be unsafe: the helper clamps the overrun to the red zone it
 * allocated, so the bytes written are always the helper's own slack and never
 * anyone else's chunk header.
 */
struct steamrpc_overrun_params
{
    uint32_t _ret;          /* helper: bytes it actually wrote past the end */
    uint32_t len;           /* bytes the client declares the buffer to be */
    uint32_t past;          /* bytes past the end to write; clamped to GUARD */
    uint32_t pad;
    unsigned char *buf;     /* IN | OUT, `len` bytes as far as the wire knows */
};

#pragma pack(pop)

#endif /* __STEAMRPC_WIRE_H */
