/*
 * host_worker.c -- NATIVE ppc64le side of the fence-callback experiment.
 *
 * This is a stand-in for vkd3d-proton's `vkd3d_fence_worker_main`: a pthread
 * created by a native host library, living inside the FEX process, that is
 * completely decoupled from any guest->host call. It is never a FEX guest
 * thread: FEX's `thread_local ThreadStateObject* ThreadObject` is null on it,
 * which is precisely the condition that makes
 * Thunks.cpp:231 ERROR_AND_DIE ("Thunked library attempted to invoke guest
 * callback asynchronously") fire if such a thread tries to enter guest code.
 *
 * It is injected with LD_PRELOAD into the FEX binary rather than through a
 * thunk, because the property under test -- can a native thread wake a parked
 * emulated thread through the kernel -- does not involve the thunk dispatch
 * path at all. See fence-callback.md for what that does and does not cover.
 *
 * What it does:
 *   1. waits for the guest to publish $FENCE_EXP_DIR/rv.txt containing the
 *      address of the guest's control block,
 *   2. reads/writes that guest memory directly (same address space),
 *   3. write(2)s the guest's own eventfds -- byte-for-byte what
 *      vkd3d_native_sync_handle_signal() does,
 *   4. or FUTEX_WAKEs guest memory, or tgkill()s a guest thread,
 *   5. or, as a negative control, branches straight into guest code.
 *
 * Build:  gcc -shared -fPIC -O2 -pthread -o libfencehost.so host_worker.c
 * Use:    LD_PRELOAD=.../libfencehost.so FEX ./guest_waiter <phase>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <signal.h>
#include <time.h>
#include <linux/futex.h>

#include "fence_ctrl.h"

_Static_assert(sizeof(struct fence_ctrl) == 1448, "host fence_ctrl layout");

static struct fence_ctrl *C;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define HLOG(...) do { fprintf(stderr, "HOST: " __VA_ARGS__); fflush(stderr); } while (0)

static int host_tid(void) { return (int)syscall(SYS_gettid); }

/* One writer thread per fd -- vkd3d has one fence worker, but fanning out is
   the harsher test of whether FEX minds unknown threads doing syscalls. */
struct wt { int idx; };

/* NEGATIVE CONTROL knob: FENCE_HOST_DROP=k makes each writer silently skip k
   of its writes. The guest's "every write is accounted for" check must fail.
   Without this the no-lost-wakeup claim would be untested. */
static int drop_writes = 0;

static void *writer(void *a) {
  struct wt *w = (struct wt *)a;
  int i = w->idx;
  int fd = C->fds[i];
  uint64_t one = 1;
  int dropped = 0;
  for (int r = 0; r < C->rounds; r++) {
    if (dropped < drop_writes) { dropped++; continue; }
    C->signal_ts[i] = now_ns();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* THE call under test: identical to vkd3d_native_sync_handle_signal(). */
    ssize_t n = write(fd, &one, 8);
    if (n != 8) {
      __atomic_add_fetch((uint64_t *)&C->host_errors, 1, __ATOMIC_SEQ_CST);
      HLOG("write fd=%d failed: %s\n", fd, strerror(errno));
      return NULL;
    }
    __atomic_add_fetch((uint64_t *)&C->host_writes, 1, __ATOMIC_SEQ_CST);
    if (C->pingpong) {
      /* strict handshake: do not race ahead of the guest's timestamp read */
      uint64_t want = (uint64_t)r + 1;
      uint64_t spins = 0;
      while (__atomic_load_n((uint64_t *)&C->ack[i], __ATOMIC_SEQ_CST) < want) {
        if (++spins > 200000000ull) { HLOG("pingpong stall at round %d\n", r); return NULL; }
        if (C->stop) return NULL;
      }
    }
  }
  return NULL;
}

static void do_eventfds(void) {
  pthread_t th[FENCE_MAX_FD];
  struct wt wa[FENCE_MAX_FD];
  int n = 0;
  for (int i = 0; i < C->nfd; i++) {
    if (i == C->nosignal_idx) { HLOG("deliberately NOT signalling fd[%d]=%d (negative control)\n", i, C->fds[i]); continue; }
    wa[n].idx = i;
    if (pthread_create(&th[n], NULL, writer, &wa[n]) != 0) { HLOG("pthread_create failed\n"); break; }
    n++;
  }
  HLOG("%d writer threads started, %d rounds each\n", n, C->rounds);
  for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
}

static void do_futex(void) {
  int32_t *fw = (int32_t *)(uintptr_t)C->futex_word_addr;
  int op = C->do_futex_shared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE;
  /* give the guest time to actually get into FUTEX_WAIT */
  struct timespec s = {0, 300 * 1000000};
  nanosleep(&s, NULL);
  HLOG("futex word at %p currently %d; storing 0x5A5A and waking (op=%d)\n", (void *)fw, *fw, op);
  __atomic_store_n(fw, 0x5A5A, __ATOMIC_SEQ_CST);
  C->futex_ts = now_ns();
  long r = syscall(SYS_futex, fw, op, 0x7fffffff, NULL, NULL, 0);
  HLOG("FUTEX_WAKE returned %ld (woken) errno=%d\n", r, r < 0 ? errno : 0);
  if (r < 0) __atomic_add_fetch((uint64_t *)&C->host_errors, 1, __ATOMIC_SEQ_CST);
}

static void do_signal(void) {
  struct timespec s = {0, 300 * 1000000};
  nanosleep(&s, NULL);
  HLOG("tgkill(pid=%d, tid=%d, SIGUSR1) from native thread %d\n", C->pid, C->target_tid, host_tid());
  C->sig_ts = now_ns();
  long r = syscall(SYS_tgkill, C->pid, C->target_tid, SIGUSR1);
  HLOG("tgkill returned %ld errno=%d\n", r, r < 0 ? errno : 0);
  if (r < 0) __atomic_add_fetch((uint64_t *)&C->host_errors, 1, __ATOMIC_SEQ_CST);
}

/* NEGATIVE CONTROL: a native ppc64le thread branching directly into x86-64
   guest code. This is what a real host->guest callback would be if it did not
   go through FEX. It must not survive. */
static void do_callguest(void) {
  struct timespec s = {0, 300 * 1000000};
  nanosleep(&s, NULL);
  void (*fn)(void) = (void (*)(void))(uintptr_t)C->guestfn_addr;
  HLOG("NEGATIVE CONTROL: native thread %d branching to guest code at %p\n", host_tid(), (void *)fn);
  fn();
  HLOG("NEGATIVE CONTROL RETURNED -- this should be unreachable\n");
}

static void *controller(void *_) {
  (void)_;
  const char *dir = getenv("FENCE_EXP_DIR");
  if (!dir) dir = "/tmp/fence-exp";
  if (getenv("FENCE_HOST_DROP")) drop_writes = atoi(getenv("FENCE_HOST_DROP"));
  char path[512];
  snprintf(path, sizeof(path), "%s/rv.txt", dir);

  HLOG("native worker up, tid=%d, waiting for %s\n", host_tid(), path);

  unsigned long long magic = 0, ctrl = 0;
  int pid = 0;
  char phase[64] = {0};
  for (int i = 0; i < 6000; i++) {   /* up to 60 s */
    FILE *f = fopen(path, "r");
    if (f) {
      if (fscanf(f, "magic=%llx\nctrl=%llx\npid=%d\nphase=%63s", &magic, &ctrl, &pid, phase) == 4) { fclose(f); break; }
      fclose(f);
    }
    struct timespec s = {0, 10 * 1000000};
    nanosleep(&s, NULL);
  }
  if (!ctrl) { HLOG("rendezvous file never appeared/parsed; giving up\n"); return NULL; }

  /* LD_PRELOAD is inherited by every process FEX spawns (FEXServer, and any
     re-exec of FEX itself). Only the process that actually contains the guest
     may touch the control block -- in any other process that address is either
     unmapped or, worse, someone else's memory. */
  if ((int)getpid() != pid) {
    HLOG("not the guest's process (me=%d guest=%d); standing down\n", (int)getpid(), pid);
    return NULL;
  }

  C = (struct fence_ctrl *)(uintptr_t)ctrl;
  HLOG("rendezvous: ctrl=%p pid=%d phase=%s (I am pid %d, native tid %d)\n",
       (void *)C, pid, phase, (int)getpid(), host_tid());

  /* Read guest memory from a native thread. */
  if (C->magic != FENCE_MAGIC) {
    HLOG("BAD MAGIC in guest memory: got %llx want %llx -- cannot read guest memory\n",
         (unsigned long long)C->magic, (unsigned long long)FENCE_MAGIC);
    return NULL;
  }
  HLOG("read guest control block OK (magic verified, nfd=%d rounds=%d)\n", C->nfd, C->rounds);

  while (!__atomic_load_n((uint64_t *)&C->go, __ATOMIC_SEQ_CST)) {
    struct timespec s = {0, 1 * 1000000};
    nanosleep(&s, NULL);
  }

  C->host_tid = (uint64_t)host_tid();
  C->host_probe = 0xDEADBEEFCAFEULL;      /* native store into guest memory */
  __atomic_store_n((uint64_t *)&C->host_started, 1, __ATOMIC_SEQ_CST);

  if (C->do_callguest)      do_callguest();
  else if (C->do_futex)     do_futex();
  else if (C->do_signal)    do_signal();
  else                      do_eventfds();

  __atomic_store_n((uint64_t *)&C->host_done, 1, __ATOMIC_SEQ_CST);
  HLOG("done: writes=%llu errors=%llu\n",
       (unsigned long long)C->host_writes, (unsigned long long)C->host_errors);
  return NULL;
}

__attribute__((constructor)) static void fence_host_init(void) {
  if (getenv("FENCE_HOST_OFF")) return;
  pthread_t t;
  if (pthread_create(&t, NULL, controller, NULL) != 0) {
    fprintf(stderr, "HOST: pthread_create failed: %s\n", strerror(errno));
    return;
  }
  pthread_detach(t);
}
