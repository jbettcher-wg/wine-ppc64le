/*
 * ntsync userspace fast-path self test.
 *
 * A wrong sync fast path does not crash.  It produces a rare missed or
 * spurious wakeup that surfaces hours later as a hang and gets blamed on
 * something else.  So this test does not check that the fast path is fast; it
 * checks the two things whose failure is invisible:
 *
 *   - conservation:  every unit released is acquired exactly once, and the
 *                    count never exceeds max and never wraps;
 *   - liveness:      no waiter is ever left asleep on a signalled semaphore.
 *
 * The fast-path code here is a deliberate copy of the one in Wine's
 * dlls/ntdll/unix/sync.c rather than a call into it, so that the protocol is
 * stated twice and a divergence shows up as a test failure.
 *
 * Build:  gcc -O2 -pthread -o fastpath-test fastpath-test.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/ntsync.h>

#define SHM_VERSION      1
#define COUNT_MASK       0x00000000ffffffffull
#define WAITER_MASK      0x1fffffff00000000ull
#define F_HOLD           0x2000000000000000ull
#define F_WAIT_ALL       0x4000000000000000ull
#define F_NO_FASTPATH    0x8000000000000000ull
#define F_NO_TOUCH       (F_WAIT_ALL | F_NO_FASTPATH)
#define F_NO_RELEASE     (F_HOLD | F_WAIT_ALL | F_NO_FASTPATH)

struct ntsync_shm {
    unsigned long long state;
    unsigned int max, type, version, reserved[27];
};

static int device_fd = -1;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while (0)

/* ------------------------------------------------------------------ helpers */

static int create_sem(unsigned int count, unsigned int max)
{
    struct ntsync_sem_args args = { .count = count, .max = max };
    int fd = ioctl(device_fd, NTSYNC_IOC_CREATE_SEM, &args);
    if (fd < 0) { perror("CREATE_SEM"); exit(1); }
    return fd;
}

static struct ntsync_shm *map_sem(int fd)
{
    struct ntsync_shm *shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) return NULL;
    return shm;
}

static unsigned int sem_read(int fd)
{
    struct ntsync_sem_args args = {0};
    if (ioctl(fd, NTSYNC_IOC_SEM_READ, &args) < 0) { perror("SEM_READ"); exit(1); }
    return args.count;
}

/* exactly the acquire fast path from dlls/ntdll/unix/sync.c */
static int fast_acquire(struct ntsync_shm *shm)
{
    unsigned long long old = atomic_load_explicit((_Atomic unsigned long long *)&shm->state,
                                                  memory_order_relaxed);
    for (;;) {
        if (old & F_NO_TOUCH) return 0;
        if (!(old & COUNT_MASK)) return 0;
        if (atomic_compare_exchange_weak_explicit((_Atomic unsigned long long *)&shm->state,
                                                  &old, old - 1,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed))
            return 1;
    }
}

/* exactly the release fast path from dlls/ntdll/unix/sync.c.
 * returns 1 if it did the release, 0 if the caller must use the ioctl */
static int fast_release(int fd, struct ntsync_shm *shm, unsigned int n, unsigned int *prev)
{
    unsigned long long old, val;
    unsigned int p;

    if (!n) return 0;
    old = atomic_load_explicit((_Atomic unsigned long long *)&shm->state, memory_order_relaxed);
    for (;;) {
        if (old & F_NO_RELEASE) return 0;
        p = (unsigned int)(old & COUNT_MASK);
        if ((unsigned long long)p + n > shm->max) return 0;
        val = (old & ~COUNT_MASK) | (p + n);
        if (atomic_compare_exchange_weak_explicit((_Atomic unsigned long long *)&shm->state,
                                                  &old, val,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed))
            break;
    }
    if (old & WAITER_MASK) {
        __u32 zero = 0;
        if (ioctl(fd, NTSYNC_IOC_SEM_RELEASE, &zero) < 0) { perror("wake"); exit(1); }
    }
    if (prev) *prev = p;
    return 1;
}

static int slow_release(int fd, unsigned int n, unsigned int *prev)
{
    __u32 args = n;
    if (ioctl(fd, NTSYNC_IOC_SEM_RELEASE, &args) < 0) return -errno;
    if (prev) *prev = args;
    return 0;
}

/* blocking acquire via the kernel; ms < 0 means wait forever */
static int slow_acquire(int fd, int ms)
{
    struct ntsync_wait_args args = {0};
    int objs = fd;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    args.objs = (uintptr_t)&objs;
    args.count = 1;
    args.owner = 1;
    args.index = ~0u;
    if (ms < 0) args.timeout = ~(unsigned long long)0;
    else args.timeout = (unsigned long long)now.tv_sec * 1000000000ull + now.tv_nsec
                        + (unsigned long long)ms * 1000000ull;

    if (ioctl(device_fd, NTSYNC_IOC_WAIT_ANY, &args) < 0) {
        if (errno == ETIMEDOUT) return 0;
        perror("WAIT_ANY"); exit(1);
    }
    return 1;
}

/* --------------------------------------------------------------- test: ABI */

static void test_abi(void)
{
    int fd = create_sem(3, 10);
    struct ntsync_shm *shm = map_sem(fd);

    CHECK(shm != NULL, "mmap failed: %s -- is the fast-path module loaded?", strerror(errno));
    if (!shm) { close(fd); return; }

    CHECK(shm->version == SHM_VERSION, "version %u", shm->version);
    CHECK(shm->type == 0, "type %u (want NTSYNC_TYPE_SEM)", shm->type);
    CHECK(shm->max == 10, "max %u", shm->max);
    CHECK((shm->state & COUNT_MASK) == 3, "initial count %llu", shm->state & COUNT_MASK);
    CHECK(!(shm->state & F_NO_TOUCH), "flags set on a fresh semaphore: %#llx", shm->state);

    /* the kernel and the page must agree, in both directions */
    CHECK(sem_read(fd) == 3, "SEM_READ disagrees with the page");
    CHECK(fast_acquire(shm), "fast acquire of a signalled semaphore failed");
    CHECK(sem_read(fd) == 2, "SEM_READ %u after a fast acquire, want 2", sem_read(fd));
    slow_release(fd, 1, NULL);
    CHECK((shm->state & COUNT_MASK) == 3, "page %llu after an ioctl release, want 3",
          shm->state & COUNT_MASK);

    /* a release that would land on max must refuse and leave the count alone */
    {
        unsigned int prev = 0;
        int fd2 = create_sem(0, 1);
        struct ntsync_shm *s2 = map_sem(fd2);
        CHECK(fast_release(fd2, s2, 1, &prev) && prev == 0, "fast release to max refused");
        CHECK((s2->state & COUNT_MASK) == 1, "count %llu after a fast release to max",
              s2->state & COUNT_MASK);
        CHECK(!fast_release(fd2, s2, 1, &prev), "fast release past max was allowed");
        CHECK(slow_release(fd2, 1, &prev) == -EOVERFLOW, "release past max was not EOVERFLOW");
        CHECK(fast_acquire(s2), "fast acquire of the max unit failed");
        munmap(s2, sizeof(*s2)); close(fd2);
    }

    /* mutexes and events must be permanently off limits */
    {
        struct ntsync_event_args ea = { .manual = 1, .signaled = 1 };
        int efd = ioctl(device_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
        struct ntsync_shm *es = map_sem(efd);
        CHECK(es && (es->state & F_NO_FASTPATH), "event page is not marked NO_FASTPATH");
        if (es) munmap(es, sizeof(*es));
        close(efd);
    }

    /* bad mmap requests must be refused rather than accommodated */
    CHECK(mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) == MAP_FAILED,
          "MAP_PRIVATE was accepted");
    CHECK(mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1) == MAP_FAILED,
          "a nonzero offset was accepted");

    munmap(shm, sizeof(*shm));
    close(fd);
    printf("  abi: done\n");
}

/* ------------------------------------------------- test: conservation/excl */

struct conserve {
    int fd;
    struct ntsync_shm *shm;
    unsigned long iters;
    _Atomic long produced, consumed, over_max;
    _Atomic int stop;
};

static void *producer(void *arg)
{
    struct conserve *c = arg;
    unsigned int seed = (unsigned int)(uintptr_t)pthread_self();

    for (unsigned long i = 0; i < c->iters; i++) {
        /* mix the two paths so they race each other, not just themselves */
        if (rand_r(&seed) & 1) {
            if (!fast_release(c->fd, c->shm, 1, NULL)) {
                if (slow_release(c->fd, 1, NULL) < 0) continue;
            }
        } else {
            if (slow_release(c->fd, 1, NULL) < 0) continue;
        }
        atomic_fetch_add(&c->produced, 1);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    struct conserve *c = arg;
    unsigned int seed = (unsigned int)(uintptr_t)pthread_self() ^ 0x5eed;

    while (!atomic_load(&c->stop)) {
        int got;

        if (rand_r(&seed) & 1) got = fast_acquire(c->shm);
        else got = slow_acquire(c->fd, 2);

        /* the count must never be seen outside [0, max], in either direction */
        if ((c->shm->state & COUNT_MASK) > c->shm->max) atomic_fetch_add(&c->over_max, 1);

        if (!got) continue;
        atomic_fetch_add(&c->consumed, 1);
    }
    return NULL;
}

static void test_conservation(unsigned int max, int nprod, int ncons, unsigned long iters)
{
    struct conserve c = {0};
    pthread_t p[64], q[64];
    long leftover;

    c.fd = create_sem(0, max);
    c.shm = map_sem(c.fd);
    if (!c.shm) { printf("  (skipped, no mapping)\n"); close(c.fd); return; }
    c.iters = iters;

    for (int i = 0; i < ncons; i++) pthread_create(&q[i], NULL, consumer, &c);
    for (int i = 0; i < nprod; i++) pthread_create(&p[i], NULL, producer, &c);
    for (int i = 0; i < nprod; i++) pthread_join(p[i], NULL);

    /* let the consumers drain, then stop them */
    for (int i = 0; i < 200; i++) {
        if (atomic_load(&c.consumed) >= atomic_load(&c.produced)) break;
        usleep(10000);
    }
    atomic_store(&c.stop, 1);
    for (int i = 0; i < ncons; i++) pthread_join(q[i], NULL);

    leftover = (long)(c.shm->state & COUNT_MASK);
    CHECK(atomic_load(&c.consumed) + leftover == atomic_load(&c.produced),
          "conservation: produced %ld, consumed %ld, left %ld",
          (long)c.produced, (long)c.consumed, leftover);
    CHECK(leftover >= 0 && (unsigned long)leftover <= max,
          "count %ld outside [0, %u] -- underflow or overshoot", leftover, max);
    CHECK(!(c.shm->state & WAITER_MASK), "waiters left behind: %#llx", c.shm->state);
    CHECK(!atomic_load(&c.over_max), "the count was seen above max %d times", (int)c.over_max);

    printf("  max=%u prod=%d cons=%d: produced %ld consumed %ld left %ld\n",
           max, nprod, ncons, (long)c.produced, (long)c.consumed, leftover);

    munmap(c.shm, sizeof(*c.shm));
    close(c.fd);
}

/* ------------------------------------------------- test: mutual exclusion */

struct mutex_test {
    int fd;
    struct ntsync_shm *shm;
    _Atomic long in_cs, cs_max, rounds, saw_hold;
    _Atomic int stop;
};

/*
 * The one that matters.  A semaphore of max 1, held and returned, is a mutex;
 * if the fast path ever hands the same unit to two threads this counter goes
 * above 1 and stays there.  Half the acquires and half the releases go through
 * the kernel so that the two paths race each other rather than themselves, and
 * the contention keeps waiters queued, which is exactly the state in which the
 * module holds a unit on loan -- the window NTSYNC_F_HOLD exists to close.
 */
static void *mutex_thread(void *arg)
{
    struct mutex_test *m = arg;
    unsigned int seed = (unsigned int)(uintptr_t)pthread_self() ^ 0x0d1u;

    while (!atomic_load(&m->stop)) {
        long n, mx;
        int got;

        if (m->shm->state & F_HOLD) atomic_fetch_add(&m->saw_hold, 1);

        if (rand_r(&seed) & 1) got = fast_acquire(m->shm);
        else got = slow_acquire(m->fd, 50);
        if (!got) continue;

        n = atomic_fetch_add(&m->in_cs, 1) + 1;
        mx = atomic_load(&m->cs_max);
        while (n > mx && !atomic_compare_exchange_weak(&m->cs_max, &mx, n)) ;
        for (int k = rand_r(&seed) % 40; k > 0; k--) __asm__ volatile("" ::: "memory");
        atomic_fetch_sub(&m->in_cs, 1);
        atomic_fetch_add(&m->rounds, 1);

        if (!fast_release(m->fd, m->shm, 1, NULL))
            slow_release(m->fd, 1, NULL);
    }
    return NULL;
}

static void test_mutual_exclusion(int nthreads, int seconds)
{
    struct mutex_test m = {0};
    pthread_t t[64];

    m.fd = create_sem(1, 1);
    m.shm = map_sem(m.fd);
    if (!m.shm) { printf("  (skipped, no mapping)\n"); close(m.fd); return; }

    for (int i = 0; i < nthreads; i++) pthread_create(&t[i], NULL, mutex_thread, &m);
    sleep(seconds);
    atomic_store(&m.stop, 1);
    for (int i = 0; i < nthreads; i++) pthread_join(t[i], NULL);

    CHECK(atomic_load(&m.cs_max) <= 1, "a unit was handed to %ld threads at once",
          (long)m.cs_max);
    CHECK((m.shm->state & COUNT_MASK) == 1, "count %llu at rest, want 1",
          m.shm->state & COUNT_MASK);
    CHECK(!(m.shm->state & WAITER_MASK), "waiters left behind: %#llx", m.shm->state);
    CHECK(atomic_load(&m.rounds) > 1000, "only %ld rounds -- test proved little",
          (long)m.rounds);
    printf("  mutex: %ld rounds, peak holders %ld, loan window seen %ld times\n",
           (long)m.rounds, (long)m.cs_max, (long)m.saw_hold);

    munmap(m.shm, sizeof(*m.shm));
    close(m.fd);
}

/* -------------------------------------------------------- test: no lost wakeup */

struct wakeup {
    int fd;
    struct ntsync_shm *shm;
    unsigned long iters;
    _Atomic int timeouts, done;
};

static void *waker(void *arg)
{
    struct wakeup *w = arg;
    unsigned int seed = 12345;

    for (unsigned long i = 0; i < w->iters; i++) {
        /* Spend a random moment doing nothing so the sleeper really does get
         * into the kernel and queue itself for some of these. */
        for (int k = rand_r(&seed) % 200; k > 0; k--) __asm__ volatile("" ::: "memory");
        if (!fast_release(w->fd, w->shm, 1, NULL))
            slow_release(w->fd, 1, NULL);
    }
    atomic_store(&w->done, 1);
    return NULL;
}

/*
 * The sleeper only ever blocks in the kernel.  Every release must reach it.  A
 * timeout here is the failure this whole exercise exists to prevent: the
 * semaphore was signalled and the waiter stayed asleep.
 */
static void *sleeper(void *arg)
{
    struct wakeup *w = arg;
    unsigned long got = 0;

    while (got < w->iters) {
        if (slow_acquire(w->fd, 1000)) { got++; continue; }
        if (atomic_load(&w->done) && !(w->shm->state & COUNT_MASK)) {
            /* producer finished and the semaphore really is empty */
            if (got < w->iters) atomic_fetch_add(&w->timeouts, 1);
            break;
        }
        atomic_fetch_add(&w->timeouts, 1);
        break;
    }
    return NULL;
}

static void test_no_lost_wakeup(unsigned long iters)
{
    struct wakeup w = {0};
    pthread_t a, b;

    w.fd = create_sem(0, 1u << 30);
    w.shm = map_sem(w.fd);
    if (!w.shm) { printf("  (skipped, no mapping)\n"); close(w.fd); return; }
    w.iters = iters;

    pthread_create(&b, NULL, sleeper, &w);
    pthread_create(&a, NULL, waker, &w);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    CHECK(atomic_load(&w.timeouts) == 0,
          "%d lost wakeups over %lu releases", (int)w.timeouts, iters);
    printf("  lost-wakeup: %lu releases, %d timeouts, %llu left\n",
           iters, (int)w.timeouts, w.shm->state & COUNT_MASK);

    munmap(w.shm, sizeof(*w.shm));
    close(w.fd);
}

/* ----------------------------------------------------- test: wait-all lockout */

struct waitall {
    int fd[2];
    struct ntsync_shm *shm[2];
    _Atomic int stop, rounds, saw_flag;
};

static void *waitall_thread(void *arg)
{
    struct waitall *w = arg;
    struct ntsync_wait_args args = {0};
    int objs[2] = { w->fd[0], w->fd[1] };
    struct timespec now;

    while (!atomic_load(&w->stop)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        memset(&args, 0, sizeof(args));
        args.objs = (uintptr_t)objs;
        args.count = 2;
        args.owner = 1;
        args.index = ~0u;
        args.timeout = (unsigned long long)now.tv_sec * 1000000000ull + now.tv_nsec + 200000ull;
        if (ioctl(device_fd, NTSYNC_IOC_WAIT_ALL, &args) == 0)
            atomic_fetch_add(&w->rounds, 1);
    }
    return NULL;
}

static void *waitall_poker(void *arg)
{
    struct waitall *w = arg;

    while (!atomic_load(&w->stop)) {
        for (int i = 0; i < 2; i++) {
            if (w->shm[i]->state & F_WAIT_ALL) atomic_store(&w->saw_flag, 1);
            if (!fast_release(w->fd[i], w->shm[i], 1, NULL))
                slow_release(w->fd[i], 1, NULL);
            fast_acquire(w->shm[i]);
        }
    }
    return NULL;
}

/*
 * try_wake_all() checks every object and then commits to every object with no
 * way to roll back, so userspace must be locked out for the whole time a
 * wait-all is queued.  Hammer both sides at once and then check that neither
 * count ever left [0, max] -- an underflow would show as a count near 2^32.
 */
static void test_wait_all_lockout(int seconds)
{
    struct waitall w = {0};
    pthread_t a, b[4];

    for (int i = 0; i < 2; i++) {
        w.fd[i] = create_sem(0, 1u << 20);
        w.shm[i] = map_sem(w.fd[i]);
        if (!w.shm[i]) { printf("  (skipped, no mapping)\n"); return; }
    }

    pthread_create(&a, NULL, waitall_thread, &w);
    for (int i = 0; i < 4; i++) pthread_create(&b[i], NULL, waitall_poker, &w);
    sleep(seconds);
    atomic_store(&w.stop, 1);
    pthread_join(a, NULL);
    for (int i = 0; i < 4; i++) pthread_join(b[i], NULL);

    for (int i = 0; i < 2; i++) {
        unsigned long long count = w.shm[i]->state & COUNT_MASK;
        CHECK(count <= (1u << 20), "sem %d count %llu is outside [0, max] -- underflow", i, count);
        CHECK(!(w.shm[i]->state & F_WAIT_ALL), "sem %d still marked WAIT_ALL after everyone left", i);
        CHECK(!(w.shm[i]->state & WAITER_MASK), "sem %d waiters left behind: %#llx", i, w.shm[i]->state);
    }
    CHECK(atomic_load(&w.saw_flag), "the WAIT_ALL flag was never observed -- test proved nothing");
    printf("  wait-all: %d wakeups, flag observed %d, counts %llu/%llu\n",
           (int)w.rounds, (int)w.saw_flag,
           w.shm[0]->state & COUNT_MASK, w.shm[1]->state & COUNT_MASK);

    for (int i = 0; i < 2; i++) { munmap(w.shm[i], sizeof(*w.shm[i])); close(w.fd[i]); }
}

int main(void)
{
    if ((device_fd = open("/dev/ntsync", O_RDWR | O_CLOEXEC)) < 0) {
        perror("/dev/ntsync");
        return 2;
    }

    printf("abi\n");                test_abi();
    printf("conservation\n");
    test_conservation(1,        4,  4, 200000);
    test_conservation(64,       8,  8, 200000);
    test_conservation(1u << 20, 16, 16, 200000);
    printf("mutual exclusion\n");
    test_mutual_exclusion(16, 6);
    test_mutual_exclusion(64, 6);
    printf("lost wakeups\n");       test_no_lost_wakeup(300000);
    printf("wait-all lockout\n");   test_wait_all_lockout(5);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
