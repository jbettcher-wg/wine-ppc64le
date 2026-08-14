/* Does vkd3d-proton's native HANDLE convention hold up, and does it agree with
 * DXVK's?
 *
 * THE TWO DEFECTS THIS WAS WRITTEN AGAINST, both in upstream's
 * `vkd3d_native_sync_handle_wrap` (include/private/vkd3d_native_sync_handle.h):
 *
 *   handle.fd = (int)(intptr_t)os_handle;
 *   // Treats FD 0 as invalid. FD 0 is technically valid since it's STDIN...
 *   handle.type = os_handle && handle.fd >= 0 ? type : ..._TYPE_NONE;
 *
 *   1. COLLISION. A bare fd is indistinguishable from every other HANDLE the
 *      library is handed. In this port a D3D12 caller can be an emulated
 *      x86-64 PE whose HANDLE crosses the thunk untranslated, so what arrives
 *      is a Wine object -- a small integer sitting squarely in the range of
 *      live fd numbers. `SetEventOnCompletion(f, v, (HANDLE)0x2c)` becomes
 *      `write(44, &one, 8)`: eight bytes into whatever the process has open
 *      there. Nothing reports it.
 *   2. FD 0. An eventfd that lands on fd 0 is dropped -- upstream says so in
 *      the comment above and then works around it with a dup() dance in
 *      `GetFrameLatencyEvent` and in `tests/d3d12_crosstest.h`. The
 *      GetFrameLatencyEvent workaround is itself broken: it returns the number
 *      of a descriptor it has just closed.
 *
 * WHAT IS COVERED
 *   A  a tagged handle wraps to a valid handle naming the right fd
 *   B  signal -> acquire round-trips through the wrapped handle
 *   C  fd 0 is a usable event
 *   D  a Wine-shaped handle whose value is a LIVE FD is refused, and nothing
 *      is written into that fd
 *   E  NULL is still refused, and the create/destroy path still works
 *   F  vkd3d's tag is bit-identical to DXVK's, and each side decodes the
 *      other's handle -- the point of the whole exercise
 *
 * HOW IT FAILS BEFORE THE CHANGE. probes/tagged_handle_run.sh builds this
 * twice: once against the patched tree, and once against the header as it is
 * at the pinned upstream commit, extracted with `git show`. That is not a
 * simulation of the old behaviour, it IS the old behaviour, compiled. The
 * upstream build must fail cases C and D; the patched build must pass all six.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/eventfd.h>

/* vkd3d_native_sync_handle.h is included from vkd3d_private.h, after the
 * Windows compatibility layer and the logging macros. Reproduce just that
 * much: this probe deliberately compiles the REAL header rather than a copy,
 * because a copy would be the thing most likely to disagree. */
#include "vkd3d_windows.h"

#define VKD3D_UNUSED
#define ERR(...)   do { if (getenv("PROBE_VERBOSE")) fprintf(stderr, "  err: " __VA_ARGS__); } while (0)
#define TRACE(...) do { } while (0)
#define WARN(...)  do { } while (0)
#define DXGI_MAX_SWAP_CHAIN_BUFFERS 16

#include "vkd3d_native_sync_handle.h"

/* DXVK's copy of the tag, for case F. Only reached when the run script finds a
 * dxvk-ppc64le checkout beside this one; the comparison is the entire reason
 * the constant is allowed to exist in more than one place. */
#ifdef HAVE_DXVK_NATIVE_EVENT
#include <dxvk_native_event.h>
#endif

/* Present only after the patch. Its absence is what makes the upstream build
 * of this probe exercise upstream's convention rather than failing to compile:
 * the cases fall back to the bare cast the library then requires. */
#ifdef __VKD3D_NATIVE_EVENT_HANDLE_H
#define TAGGED 1
#else
#define TAGGED 0
static inline HANDLE vkd3d_native_event_handle_from_fd(int fd)
{
    return (HANDLE)(intptr_t)fd;
}
#endif

static int failures;

static void check(const char *what, bool ok, const char *detail)
{
    if (!ok)
        failures++;
    printf("  %-56s %s%s%s\n", what, ok ? "ok" : "FAIL",
           detail ? "  " : "", detail ? detail : "");
}

int main(void)
{
    char detail[160];

    printf("vkd3d native sync handles  (tagged convention: %s)\n\n",
           TAGGED ? "PRESENT" : "ABSENT -- this is upstream");

    /* ================================================================= A */
    printf("A. a handle minted the library's own way wraps to a valid handle\n");
    {
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        HANDLE h = vkd3d_native_event_handle_from_fd(fd);
        vkd3d_native_sync_handle sh =
            vkd3d_native_sync_handle_wrap(h, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT);

        snprintf(detail, sizeof(detail), "fd=%d wrapped fd=%d", fd, sh.fd);
        check("wrap() accepts it", vkd3d_native_sync_handle_is_valid(sh), detail);
        check("and it names the right fd", sh.fd == fd, detail);

        /* ============================================================= B */
        printf("B. signal -> acquire round-trips\n");
        check("signal", vkd3d_native_sync_handle_signal(sh) == S_OK, NULL);
        check("acquire returns within 1000 ms",
              vkd3d_native_sync_handle_acquire_timeout(sh, 1000), NULL);
        check("and a second acquire times out (it was consumed)",
              !vkd3d_native_sync_handle_acquire_timeout(sh, 50), NULL);

        vkd3d_native_sync_handle_destroy(sh);
    }

    /* ================================================================= C
     * fd 0. Force it: close stdin, then the next eventfd lands there. Under
     * the bare-fd convention the resulting HANDLE is NULL and wrap() reports
     * NONE, which is upstream's documented behaviour and a dropped event. */
    printf("C. an eventfd on fd 0 is a usable event\n");
    {
        int saved = dup(0);
        close(0);
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

        if (fd != 0) {
            check("eventfd landed on fd 0", false, "not fd 0 -- case is vacuous");
        } else {
            HANDLE h = vkd3d_native_event_handle_from_fd(0);
            vkd3d_native_sync_handle sh =
                vkd3d_native_sync_handle_wrap(h, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT);

            snprintf(detail, sizeof(detail), "HANDLE=%p type=%d fd=%d",
                     h, (int) sh.type, sh.fd);
            /* stdout still points at the terminal; only fd 0 was taken. */
            check("the handle for fd 0 is not NULL", h != NULL, detail);
            check("wrap() accepts fd 0", vkd3d_native_sync_handle_is_valid(sh), detail);
            check("and it round-trips",
                  vkd3d_native_sync_handle_signal(sh) == S_OK &&
                  vkd3d_native_sync_handle_acquire_timeout(sh, 1000), detail);
        }

        if (fd >= 0)
            close(fd);
        if (saved >= 0) { dup2(saved, 0); close(saved); }
    }

    /* ================================================================= D
     * The collision. A Wine HANDLE is a small integer; give the library the
     * number of a live pipe descriptor and require that (a) wrap() refuses it
     * and (b) nothing is written into the pipe. Under the bare-fd convention
     * both fail, and the second failure is data corruption in someone else's
     * file. */
    printf("D. a Wine-shaped handle that collides with a live fd is refused\n");
    {
        int pipefd[2] = { -1, -1 };
        check("pipe()", pipe(pipefd) == 0, NULL);

        if (pipefd[1] >= 0) {
            HANDLE h = (HANDLE)(intptr_t) pipefd[1];      /* exactly a Wine handle */
            vkd3d_native_sync_handle sh =
                vkd3d_native_sync_handle_wrap(h, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT);

            snprintf(detail, sizeof(detail), "handle=%p type=%d fd=%d",
                     h, (int) sh.type, sh.fd);
            check("wrap() refuses it", !vkd3d_native_sync_handle_is_valid(sh), detail);

            /* Signal it anyway, exactly as the fence worker would
             * (command.c:1150), and see whether the pipe receives anything. */
            vkd3d_native_sync_handle_signal(sh);

            struct pollfd pfd = { pipefd[0], POLLIN, 0 };
            int rc = poll(&pfd, 1, 100);
            snprintf(detail, sizeof(detail), "poll=%d revents=0x%x", rc, pfd.revents);
            check("and nothing was written into that fd", rc == 0, detail);
        }

        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
    }

    /* ================================================================= E */
    printf("E. what must not change\n");
    {
        vkd3d_native_sync_handle sh =
            vkd3d_native_sync_handle_wrap(NULL, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT);
        check("NULL is still refused", !vkd3d_native_sync_handle_is_valid(sh), NULL);
        check("and signalling it still returns E_FAIL",
              vkd3d_native_sync_handle_signal(sh) == E_FAIL, NULL);

        /* The library's own create path never went through wrap() and must be
         * untouched by any of this -- it is how the frame-latency semaphore
         * and the MULTI_ANY temporary event are made. */
        vkd3d_native_sync_handle own;
        HRESULT hr = vkd3d_native_sync_handle_create(
            1, VKD3D_NATIVE_SYNC_HANDLE_TYPE_SEMAPHORE, &own);
        check("vkd3d_native_sync_handle_create still works", hr == S_OK, NULL);
        check("its semaphore counts", vkd3d_native_sync_handle_acquire_timeout(own, 500), NULL);
        check("eq() on a copy of itself", vkd3d_native_sync_handle_eq(own, own), NULL);
        vkd3d_native_sync_handle_destroy(own);
    }

    /* ================================================================= F */
    printf("F. the tag agrees with DXVK's\n");
#if TAGGED && defined(HAVE_DXVK_NATIVE_EVENT)
    {
        bool same = true, cross = true;
        static const int fds[] = { 0, 1, 2, 3, 7, 42, 1023, 65535, 1 << 20, 0x7fffffff };

        check("the constants are identical",
              VKD3D_NATIVE_EVENT_TAG  == DXVK_NATIVE_EVENT_TAG &&
              VKD3D_NATIVE_EVENT_MASK == DXVK_NATIVE_EVENT_MASK, NULL);

        for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
            HANDLE a = vkd3d_native_event_handle_from_fd(fds[i]);
            HANDLE b = DXVK_NATIVE_EVENT_FROM_FD(fds[i]);
            if (a != b)
                same = false;
            if (!vkd3d_native_event_handle_is_tagged(b) || !DXVK_NATIVE_EVENT_IS(a))
                cross = false;
            if (vkd3d_native_event_handle_fd(b) != fds[i] ||
                DXVK_NATIVE_EVENT_FD(a) != fds[i])
                cross = false;
        }
        check("both encode every fd identically", same, NULL);
        check("each decodes the other's handle", cross, NULL);

        /* And the property that motivated tagging in the first place. */
        check("neither mistakes a Wine handle for an event",
              !vkd3d_native_event_handle_is_tagged((HANDLE) (intptr_t) 0x2c) &&
              !DXVK_NATIVE_EVENT_IS((HANDLE) (intptr_t) 0x2c), NULL);

        /* A handle DXVK minted, wrapped by vkd3d: the actual interop claim. */
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        vkd3d_native_sync_handle sh = vkd3d_native_sync_handle_wrap(
            DXVK_NATIVE_EVENT_FROM_FD(fd), VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT);
        check("vkd3d accepts a handle minted by DXVK",
              vkd3d_native_sync_handle_is_valid(sh) && sh.fd == fd, NULL);
        check("and signalling it is visible to DXVK's own waiter",
              vkd3d_native_sync_handle_signal(sh) == S_OK &&
              dxvk_native_event_wait(DXVK_NATIVE_EVENT_FROM_FD(fd), 1000, NULL) == 1, NULL);
        vkd3d_native_sync_handle_destroy(sh);
    }
#else
    printf("  (skipped: no dxvk_native_event.h on the include path)\n");
#endif

    printf("\n%s  (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
