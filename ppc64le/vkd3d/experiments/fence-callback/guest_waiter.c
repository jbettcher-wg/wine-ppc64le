/*
 * guest_waiter.c -- EMULATED x86-64 side of the fence-callback experiment.
 *
 * Models the D3D12 application: it owns the eventfd(s) (exactly as an app does
 * when it passes an eventfd cast to a HANDLE into
 * ID3D12Fence::SetEventOnCompletion on vkd3d-proton's native path), parks
 * threads on them, and expects something else entirely to wake them.
 *
 * Built with x86_64-linux-gnu-gcc and run under FEX. It never learns that the
 * signaller is native code.
 *
 * Phases (argv[1]):
 *   pingpong  - 1 fd, N strict ping-pong rounds, per-wake latency
 *   fanout    - NFD fds x R writes blasted with no handshake; checks that the
 *               eventfd counters account for every single write (no lost wakes)
 *   load      - fanout while guest worker threads hammer the JIT
 *   poll      - waiters block in poll(2) rather than read(2)
 *   epoll     - waiters block in epoll_wait(2)
 *   futex     - guest blocks in FUTEX_WAIT on guest memory; host does the store
 *               + FUTEX_WAKE (this is the fsync shape)
 *   futexsh   - same, shared (non-private) futex ops
 *   signal    - host thread tgkill()s a parked guest thread with SIGUSR1
 *               (a genuine ASYNCHRONOUS ENTRY INTO GUEST CODE)
 *   control-nosignal - NEGATIVE CONTROL: the host is told not to signal one fd.
 *               A correct harness MUST report failure here.
 *   control-callguest - NEGATIVE CONTROL: the host thread is handed the address
 *               of guest code and branches to it directly. Expected to die.
 *
 * Exit codes: 0 pass, 2 watchdog timeout / missing wake, 3 usage/setup error.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <linux/futex.h>

#include "fence_ctrl.h"

_Static_assert(sizeof(struct fence_ctrl) == 1448, "guest fence_ctrl layout");

static struct fence_ctrl *C;
static int NFD = 1, ROUNDS = 1000;
static const char *PHASE = "pingpong";
static volatile int sig_count = 0;
static volatile uint64_t sig_recv_ts = 0;
static uint64_t *lat_ns;          /* pingpong latencies */
static volatile uint64_t got[FENCE_MAX_FD];   /* sum of eventfd counters read */
static int watchdog_secs = 25;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void die(const char *m) { fprintf(stderr, "GUEST FATAL: %s: %s\n", m, strerror(errno)); exit(3); }

/* ---- watchdog: turns a hang (the bad outcome) into a loud, catchable fail ---- */
static void *watchdog(void *_) {
  (void)_;
  uint64_t t0 = now_ns();
  for (;;) {
    struct timespec s = {0, 50 * 1000000};
    nanosleep(&s, NULL);
    if (C->stop) return NULL;
    if (now_ns() - t0 > (uint64_t)watchdog_secs * 1000000000ull) {
      fprintf(stderr, "RESULT: FAIL phase=%s reason=watchdog-timeout secs=%d\n", PHASE, watchdog_secs);
      for (int i = 0; i < NFD; i++)
        fprintf(stderr, "  fd[%d]=%d acked=%llu counted=%llu\n", i, C->fds[i],
                (unsigned long long)C->ack[i], (unsigned long long)got[i]);
      fprintf(stderr, "  host_started=%llu host_writes=%llu host_done=%llu host_tid=%llu\n",
              (unsigned long long)C->host_started, (unsigned long long)C->host_writes,
              (unsigned long long)C->host_done, (unsigned long long)C->host_tid);
      fflush(stderr);
      _exit(2);
    }
  }
}

/* ---- guest JIT load: keeps FEX busy translating/executing while we wait ---- */
static void *loadthread(void *_) {
  (void)_;
  volatile double acc = 1.0;
  uint64_t n = 0;
  while (!C->stop) {
    for (int i = 0; i < 20000; i++) acc = acc * 1.0000001 + (double)(i & 7);
    n++;
    if ((n & 63) == 0) { int fd = open("/proc/self/stat", O_RDONLY); if (fd >= 0) close(fd); }
  }
  C->guest_probe = (uint64_t)acc;
  return NULL;
}

struct warg { int idx; };

/* ---- waiter: blocking read(2) on the eventfd, exactly like a fence wait ---- */
static void *waiter_read(void *a) {
  struct warg *w = (struct warg *)a;
  int fd = C->fds[w->idx];
  uint64_t target = (uint64_t)ROUNDS;
  while (got[w->idx] < target) {
    uint64_t v = 0;
    ssize_t r = read(fd, &v, 8);
    uint64_t t = now_ns();
    if (r < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "GUEST: read fd[%d] failed: %s\n", w->idx, strerror(errno));
      return NULL;
    }
    if (r != 8) { fprintf(stderr, "GUEST: short read %zd\n", r); return NULL; }
    got[w->idx] += v;
    if (C->pingpong) {
      uint64_t st = C->signal_ts[w->idx];
      if (st && t > st) lat_ns[C->ack[w->idx] % (uint64_t)ROUNDS] = t - st;
    }
    __atomic_add_fetch((uint64_t *)&C->ack[w->idx], 1, __ATOMIC_SEQ_CST);
  }
  return NULL;
}

static void *waiter_poll(void *a) {
  struct warg *w = (struct warg *)a;
  int fd = C->fds[w->idx];
  while (got[w->idx] < (uint64_t)ROUNDS) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r = poll(&p, 1, -1);
    if (r < 0) { if (errno == EINTR) continue; fprintf(stderr, "GUEST: poll: %s\n", strerror(errno)); return NULL; }
    uint64_t v = 0;
    if (read(fd, &v, 8) == 8) got[w->idx] += v;
    __atomic_add_fetch((uint64_t *)&C->ack[w->idx], 1, __ATOMIC_SEQ_CST);
  }
  return NULL;
}

static void *waiter_epoll(void *a) {
  struct warg *w = (struct warg *)a;
  int fd = C->fds[w->idx];
  int ep = epoll_create1(0);
  if (ep < 0) die("epoll_create1");
  struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 0 };
  if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) die("epoll_ctl");
  while (got[w->idx] < (uint64_t)ROUNDS) {
    struct epoll_event out;
    int r = epoll_wait(ep, &out, 1, -1);
    if (r < 0) { if (errno == EINTR) continue; fprintf(stderr, "GUEST: epoll_wait: %s\n", strerror(errno)); close(ep); return NULL; }
    uint64_t v = 0;
    if (read(fd, &v, 8) == 8) got[w->idx] += v;
    __atomic_add_fetch((uint64_t *)&C->ack[w->idx], 1, __ATOMIC_SEQ_CST);
  }
  close(ep);
  return NULL;
}

static void sigusr1(int s) { (void)s; sig_recv_ts = now_ns(); __atomic_add_fetch((int *)&sig_count, 1, __ATOMIC_SEQ_CST); }

int main(int argc, char **argv) {
  if (argc > 1) PHASE = argv[1];
  const char *dir = getenv("FENCE_EXP_DIR");
  if (!dir) dir = "/tmp/fence-exp";

  /* The control block lives in ordinary guest anonymous memory. In FEX the
     guest address space is the host address space, so a native host thread can
     address it directly -- which is exactly what vkd3d's fence worker needs in
     order to touch the app's fd. */
  C = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (C == MAP_FAILED) die("mmap ctrl");
  memset(C, 0, sizeof(*C));
  C->magic = FENCE_MAGIC;
  C->version = 1;
  C->pid = (int)getpid();
  C->nosignal_idx = -1;
  C->pingpong = 0;

  void *(*waiter)(void *) = waiter_read;
  int nload = 0;

  if      (!strcmp(PHASE, "pingpong"))  { NFD = 1;  ROUNDS = 1000; C->pingpong = 1; }
  else if (!strcmp(PHASE, "fanout"))    { NFD = 32; ROUNDS = 200; }
  else if (!strcmp(PHASE, "load"))      { NFD = 32; ROUNDS = 200; nload = 4; }
  else if (!strcmp(PHASE, "poll"))      { NFD = 8;  ROUNDS = 100; waiter = waiter_poll; }
  else if (!strcmp(PHASE, "epoll"))     { NFD = 8;  ROUNDS = 100; waiter = waiter_epoll; }
  else if (!strcmp(PHASE, "futex"))     { NFD = 0;  ROUNDS = 0; C->do_futex = 1; }
  else if (!strcmp(PHASE, "futexsh"))   { NFD = 0;  ROUNDS = 0; C->do_futex = 1; C->do_futex_shared = 1; }
  else if (!strcmp(PHASE, "signal"))    { NFD = 1;  ROUNDS = 1; C->do_signal = 1; }
  else if (!strcmp(PHASE, "control-nosignal"))  { NFD = 4; ROUNDS = 10; C->nosignal_idx = 2; watchdog_secs = 8; }
  else if (!strcmp(PHASE, "control-callguest")) { NFD = 1; ROUNDS = 1; C->do_callguest = 1; watchdog_secs = 8; }
  else { fprintf(stderr, "unknown phase %s\n", PHASE); return 3; }

  /* stress knobs: FENCE_NFD / FENCE_ROUNDS / FENCE_LOAD override the phase */
  if (getenv("FENCE_NFD"))    { NFD = atoi(getenv("FENCE_NFD")); if (NFD > FENCE_MAX_FD) NFD = FENCE_MAX_FD; }
  if (getenv("FENCE_ROUNDS")) ROUNDS = atoi(getenv("FENCE_ROUNDS"));
  if (getenv("FENCE_LOAD"))   { nload = atoi(getenv("FENCE_LOAD")); if (nload > 8) nload = 8; }
  if (getenv("FENCE_WATCHDOG")) watchdog_secs = atoi(getenv("FENCE_WATCHDOG"));

  C->nfd = NFD;
  C->rounds = ROUNDS;
  lat_ns = calloc(ROUNDS ? ROUNDS : 1, sizeof(uint64_t));

  for (int i = 0; i < NFD; i++) {
    int fd = eventfd(0, 0);            /* blocking, counter semantics */
    if (fd < 0) die("eventfd");
    C->fds[i] = fd;
  }

  /* futex word: an int32 inside the guest mapping, past the ctrl struct */
  int32_t *fw = (int32_t *)((char *)C + 4096);
  *fw = 0;
  C->futex_word_addr = (uint64_t)(uintptr_t)fw;

  C->target_tid = (int)syscall(SYS_gettid);
  C->guestfn_addr = (uint64_t)(uintptr_t)&main;   /* real guest code address */

  if (C->do_signal) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1;
    sa.sa_flags = 0;                    /* no SA_RESTART: read() must EINTR */
    sigaction(SIGUSR1, &sa, NULL);
  }

  /* publish rendezvous */
  char path[512], tmp[512];
  snprintf(path, sizeof(path), "%s/rv.txt", dir);
  snprintf(tmp, sizeof(tmp), "%s/rv.tmp", dir);
  FILE *f = fopen(tmp, "w");
  if (!f) die("fopen rv");
  fprintf(f, "magic=%llx\nctrl=%llx\npid=%d\nphase=%s\n",
          (unsigned long long)FENCE_MAGIC, (unsigned long long)(uintptr_t)C, C->pid, PHASE);
  fclose(f);
  if (rename(tmp, path) < 0) die("rename rv");

  fprintf(stderr, "GUEST: phase=%s pid=%d tid=%d ctrl=%p nfd=%d rounds=%d\n",
          PHASE, C->pid, C->target_tid, (void *)C, NFD, ROUNDS);

  pthread_t wd;
  pthread_create(&wd, NULL, watchdog, NULL);
  pthread_t lt[8];
  for (int i = 0; i < nload; i++) pthread_create(&lt[i], NULL, loadthread, NULL);

  pthread_t th[FENCE_MAX_FD];
  struct warg wa[FENCE_MAX_FD];
  for (int i = 0; i < NFD; i++) { wa[i].idx = i; pthread_create(&th[i], NULL, waiter, &wa[i]); }

  uint64_t t_start = now_ns();
  __atomic_store_n((uint64_t *)&C->go, 1, __ATOMIC_SEQ_CST);   /* release the host */

  int rc = 0;

  if (C->do_futex) {
    /* Park in FUTEX_WAIT on guest memory. The host stores 1 and FUTEX_WAKEs. */
    int op = C->do_futex_shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    uint64_t t0 = now_ns();
    long r = syscall(SYS_futex, fw, op, 0, NULL, NULL, 0);
    uint64_t t1 = now_ns();
    int32_t v = __atomic_load_n(fw, __ATOMIC_SEQ_CST);
    fprintf(stderr, "GUEST: futex returned %ld errno=%d word=%d after %.3f ms\n",
            r, r < 0 ? errno : 0, v, (t1 - t0) / 1e6);
    if (v != 0x5A5A) { fprintf(stderr, "RESULT: FAIL phase=%s reason=futex-word-not-set word=%d\n", PHASE, v); rc = 2; }
    else fprintf(stderr, "RESULT: PASS phase=%s wake_ms=%.3f latency_us=%.1f\n", PHASE,
                 (t1 - t0) / 1e6, C->futex_ts ? (t1 - C->futex_ts) / 1e3 : -1.0);
  } else if (C->do_signal) {
    /* Park in a blocking read(); the host tgkill()s us. If FEX delivers the
       signal to guest code, our handler runs and read() returns EINTR. */
    uint64_t v = 0;
    uint64_t t0 = now_ns();
    ssize_t r = read(C->fds[0], &v, 8);
    uint64_t t1 = now_ns();
    fprintf(stderr, "GUEST: read returned %zd errno=%d sig_count=%d after %.3f ms\n",
            r, r < 0 ? errno : 0, sig_count, (t1 - t0) / 1e6);
    if (sig_count > 0)
      fprintf(stderr, "RESULT: PASS phase=signal handler_ran=1 latency_us=%.1f eintr=%d\n",
              C->sig_ts && sig_recv_ts > C->sig_ts ? (sig_recv_ts - C->sig_ts) / 1e3 : -1.0,
              (r < 0 && errno == EINTR));
    else { fprintf(stderr, "RESULT: FAIL phase=signal reason=handler-never-ran\n"); rc = 2; }
  } else {
    for (int i = 0; i < NFD; i++) pthread_join(th[i], NULL);
    uint64_t t_end = now_ns();
    uint64_t total = 0;
    int bad = 0;
    for (int i = 0; i < NFD; i++) {
      total += got[i];
      if (got[i] != (uint64_t)ROUNDS) {
        fprintf(stderr, "GUEST: fd[%d] counted %llu, expected %d\n", i,
                (unsigned long long)got[i], ROUNDS);
        bad++;
      }
    }
    uint64_t expect = (uint64_t)NFD * (uint64_t)ROUNDS;
    if (bad || total != expect) {
      fprintf(stderr, "RESULT: FAIL phase=%s reason=count-mismatch total=%llu expected=%llu bad_fds=%d\n",
              PHASE, (unsigned long long)total, (unsigned long long)expect, bad);
      rc = 2;
    } else {
      double el = (t_end - t_start) / 1e6;
      if (C->pingpong) {
        uint64_t s = 0, mx = 0, mn = ~0ull; int n = 0;
        for (int i = 0; i < ROUNDS; i++) if (lat_ns[i]) { s += lat_ns[i]; if (lat_ns[i] > mx) mx = lat_ns[i]; if (lat_ns[i] < mn) mn = lat_ns[i]; n++; }
        fprintf(stderr, "RESULT: PASS phase=%s wakes=%llu elapsed_ms=%.1f lat_us_mean=%.1f min=%.1f max=%.1f n=%d\n",
                PHASE, (unsigned long long)total, el, n ? s / (double)n / 1e3 : -1.0,
                n ? mn / 1e3 : -1.0, n ? mx / 1e3 : -1.0, n);
      } else {
        fprintf(stderr, "RESULT: PASS phase=%s wakes=%llu elapsed_ms=%.1f rate_per_s=%.0f\n",
                PHASE, (unsigned long long)total, el, total / (el / 1000.0));
      }
    }
  }

  /* Did the host's stores into guest memory land? */
  fprintf(stderr, "GUEST: host_tid=%llu host_started=%llu host_writes=%llu host_errors=%llu host_probe=%llx\n",
          (unsigned long long)C->host_tid, (unsigned long long)C->host_started,
          (unsigned long long)C->host_writes, (unsigned long long)C->host_errors,
          (unsigned long long)C->host_probe);
  if (C->host_probe != 0xDEADBEEFCAFEULL)
    fprintf(stderr, "GUEST: WARNING host_probe not observed (host thread may not have run)\n");

  __atomic_store_n((uint64_t *)&C->stop, 1, __ATOMIC_SEQ_CST);
  fflush(stderr);
  return rc;
}
