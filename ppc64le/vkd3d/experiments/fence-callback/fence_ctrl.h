/*
 * Shared control block between the EMULATED x86-64 guest waiter process and the
 * NATIVE ppc64le host worker thread living in the same FEX process.
 *
 * Layout must be identical when compiled by x86_64-linux-gnu-gcc (guest) and by
 * the native ppc64le gcc (host). Both are little-endian LP64, all members are
 * fixed-width and naturally aligned, so the layouts agree. A compile-time
 * assert on sizeof() on both sides catches any drift.
 */
#ifndef FENCE_CTRL_H
#define FENCE_CTRL_H

#include <stdint.h>

#define FENCE_MAGIC   0xF3NCEUL_BAD  /* placeholder, redefined below */
#undef  FENCE_MAGIC
#define FENCE_MAGIC   0x464E43454331ULL   /* "FNCEC1" */
#define FENCE_MAX_FD  64

struct fence_ctrl {
  uint64_t magic;
  uint64_t version;

  /* --- filled in by the guest before it publishes the rendezvous file --- */
  int32_t  pid;
  int32_t  nfd;
  int32_t  fds[FENCE_MAX_FD];
  int32_t  nosignal_idx;   /* index the host must NOT signal, or -1        */
  int32_t  rounds;         /* writes per fd                                */
  int32_t  pingpong;       /* 1 = wait for ack[i] before next round        */
  int32_t  do_futex;       /* 1 = also do the futex wake test              */
  int32_t  do_futex_shared;/* 1 = use shared (non-private) futex op        */
  int32_t  do_signal;      /* 1 = also tgkill(target_tid, SIGUSR1)         */
  int32_t  do_callguest;   /* 1 = NEGATIVE CONTROL: branch to guestfn_addr */
  int32_t  target_tid;     /* guest thread tid for the signal test         */
  int32_t  pad0;
  uint64_t futex_word_addr;/* address of an int32 in guest memory          */
  uint64_t guestfn_addr;   /* address of guest code, for the control       */

  /* --- written by the host worker, read by the guest --- */
  volatile uint64_t host_tid;
  volatile uint64_t host_started;
  volatile uint64_t host_done;
  volatile uint64_t host_writes;          /* total successful write()s     */
  volatile uint64_t host_errors;
  volatile uint64_t signal_ts[FENCE_MAX_FD]; /* CLOCK_MONOTONIC ns, stamped
                                                immediately before write() */
  volatile uint64_t futex_ts;
  volatile uint64_t sig_ts;
  volatile uint64_t host_probe;           /* host stores a pattern here so the
                                             guest can prove host stores into
                                             guest memory are visible        */

  /* --- written by the guest, read by the host --- */
  volatile uint64_t go;                   /* guest is parked and ready      */
  volatile uint64_t stop;
  volatile uint64_t ack[FENCE_MAX_FD];    /* incremented after each wake    */
  volatile uint64_t guest_probe;
};

#endif
