/*
 * emu_qpc -- the guest-side QueryPerformanceCounter fast path, ppc64le lane.
 *
 * Copyright 2026 the ppc64le port authors
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

#ifndef __WINE_EMU_QPC_H
#define __WINE_EMU_QPC_H

/*
 * WHY THIS EXISTS.  [MEASURED] 2026-08-27, Cyberpunk 2077 -benchmark:
 * KERNEL32.QueryPerformanceCounter was the single hottest guest/native
 * crossing in the whole port -- 256,638 calls a second, 14,061 a frame, each
 * one a full guest trap AND a full NtQueryPerformanceCounter syscall, ~14% of
 * all 3.54M crossings/s once NtCallbackReturn is counted with it.  On Windows
 * that call never leaves user space.  This is what makes it not leave user
 * space here either.
 *
 * THE CLOCK.  Native NtQueryPerformanceCounter used to answer with
 * CLOCK_BOOTTIME in 100 ns ticks.  The guest cannot read CLOCK_BOOTTIME
 * without a syscall, but it CAN read the POWER timebase: fastppcx86 compiles
 * x86 RDTSC to `mftb` (FEXCore/Source/Interface/Core/JIT/PPC64LE/ALUOps.cpp,
 * DEF_OP(CycleCounter)), optionally left-shifted by Config.TSCScale, which
 * SmallTSCScale sets so the guest-visible rate clears 1 GHz.
 *
 *   [MEASURED] 2026-08-27 on op4k (POWER8): __ppc_get_timebase_freq() =
 *   512000000 exactly; the guest's RDTSC ran at 1024084270 Hz measured over
 *   500 ms, and a guest RDTSC sample of 1877516249686 fell, halved, strictly
 *   between two native mftb reads taken around the run (937166223271 and
 *   940947274694).  So the guest counter is the native timebase shifted left
 *   by one, with the SAME zero -- not a rescaled or re-based clock.
 *
 * So both sides can be made to answer from the timebase, with arithmetic that
 * is provably identical rather than merely close:
 *
 *   native: qpc = mulhi64( mftb(),  multiplier )            + bias
 *   guest:  qpc = mulhi64( rdtsc(), multiplier ) >> shift   + bias
 *
 * with rdtsc() == mftb() << shift.  Substituting,
 *
 *   (((tb << s) * M) >> 64) >> s  ==  (tb * M) >> (64 + s) >> ... == (tb * M) >> 64
 *
 * -- two successive shifts of one 128-bit product, so the two expressions are
 * EQUAL for every tb, not equal to within a tick.  That matters more than the
 * crossing count: a guest reading and a native reading of the same instant
 * must not disagree, or interleaving them (which any game does, because Wine's
 * own internals call NtQueryPerformanceCounter) makes time run backwards, and
 * subtly wrong physics is worse than a slow QPC.
 *
 * multiplier = floor(2^64 * 10^7 / tb_freq).  At 512 MHz that is exactly
 * 5 * 2^56, so on this machine the conversion is exact as well as identical.
 *
 * bias is chosen once per session so that qpc starts out where CLOCK_BOOTTIME
 * is, because the raw timebase has a ~42 s head start on this machine's boot
 * and nothing should have to know that.  It wraps; the arithmetic is mod 2^64
 * on both sides, so a negative bias is stored as its two's complement and
 * added, not subtracted.
 *
 * IT LINES THE TWO CLOCKS UP; IT DOES NOT KEEP THEM LINED UP.  The timebase is
 * free-running and CLOCK_BOOTTIME is NTP-disciplined, so they drift:
 * [MEASURED] 2026-08-27 on op4k, -39.6 ppm over 60 s -- 2.4 ms a minute,
 * 144 ms an hour.  Windows has the same split for the same reason (QPC comes
 * off the TSC and is not slewed; the interrupt time is), and a game measuring
 * frame deltas cannot tell.  What CAN tell is code that converts between the
 * two domains, and there were two such places in Wine, both spelling the
 * server's clock "NtQueryPerformanceCounter" because on every other
 * architecture it was: server_wait()'s relative-to-absolute timeout conversion
 * and NtQueryTimer's RemainingTime.  Both now say server_monotonic_time()
 * (unix/sync.c), which is what they always meant.  Anything that only takes a
 * DIFFERENCE of two QPC readings -- the threadpool's timer queue, win32u's
 * event throttle -- is untouched and correct: 40 ppm of a one-second interval
 * is 40 microseconds.
 *
 * WHY NOT THE WINDOWS QpcBypassEnabled/QpcShift/QpcBias FIELDS.  They were the
 * obvious home for this and they do not fit.  Windows' bypass is a pure right
 * shift of the TSC, and 512 MHz >> k is never 10 MHz for any k, so the shift-
 * only form cannot express this machine's conversion at all; there is no field
 * in KUSER_SHARED_DATA for a multiplier.  Nor is there a compatibility reason
 * to try: Wine implements no QPC bypass on any architecture (the fields are
 * declared in include/ddk/wdm.h and written nowhere), and the guest's ntdll
 * here is a spec2thunk stub module, not Wine's x86-64 ntdll, so no existing
 * reader would honour them.  QpcBypassEnabled is therefore left 0 -- the
 * honest answer for a reader that does implement the Windows formula, since
 * that reader would compute garbage -- and the port's own parameters live in
 * the page tail past the end of KUSER_SHARED_DATA, behind a magic.
 */

#define TICKSPERSEC_QPC  10000000

/* Offset of struct emu_qpc_session inside the KUSER_SHARED_DATA page.  The
 * Windows structure ends at 0x738 and the section is a full page; 0xf00 is
 * past anything Microsoft has grown it into and stays in the same page, so
 * the guest reaches it with the same 0x7ffe0000 mapping it already has. */
#define EMU_QPC_SESSION_OFFSET  0xf00

#define EMU_QPC_MAGIC  (((ULONG64)0x51504331 << 32) | 0x50504336)  /* "QPC1PPC6" */

/* Written once per session by virtual_init_user_shared_data(); read by every
 * process's NtQueryPerformanceCounter and by the code that arms a guest
 * module.  Never rewritten, so no reader needs a seqlock. */
struct emu_qpc_session
{
    ULONG64 magic;       /* EMU_QPC_MAGIC once the rest is valid */
    ULONG64 multiplier;  /* floor(2^64 * 10^7 / tb_freq) */
    ULONG64 bias;        /* added mod 2^64 */
    ULONG64 tb_freq;     /* host timebase frequency, Hz */
    ULONG64 qpc_freq;    /* what QueryPerformanceFrequency answers */
};

/*
 * The per-module block a spec2thunk guest image exports as __wine_thunk_qpc.
 * It lives in the guest image's writable data, which is why it is per module
 * and per process rather than shared: the multiplier the GUEST needs depends
 * on the emulator's TSC scale, which is a property of this process's bridge
 * configuration, while the session block above is a property of the machine.
 *
 * OFFSETS ARE ABI.  tools/spec2thunk emits x86-64 that addresses these fields
 * by RIP-relative displacement, and tools/spec2thunk/spec2thunk-check re-reads
 * the same numbers; the C_ASSERTs beside this structure are what keeps the two
 * spellings from drifting.
 *
 * enabled is 0 in the image as linked, so a guest module loaded by a host that
 * knows nothing about this takes the trap on every call, which is exactly the
 * old behaviour.  Arming is one store, last, after everything else is in
 * place.
 *
 * tsc_sample is how the host learns the emulator's TSC scale without asking
 * the emulator anything: a DISARMED fast-path stub reads RDTSC, stores it
 * here and falls through to its trap, so by the time the trap dispatcher runs
 * it is holding a guest-visible counter reading a few microseconds old.  One
 * native mftb() then names the shift, because the two counters share a zero.
 * An armed stub never writes it -- the field would otherwise be a cache line
 * fifteen game threads fight over at 256k stores a second.
 */
struct emu_qpc_guest
{
    ULONG64 magic;       /* +0  EMU_QPC_MAGIC, stamped into the image */
    ULONG64 multiplier;  /* +8  the session multiplier, verbatim */
    ULONG64 bias;        /* +16 the session bias, verbatim */
    UCHAR   enabled;     /* +24 the arming flag; the stubs' only branch */
    UCHAR   shift;       /* +25 the emulator's TSC scale */
    UCHAR   pad[6];      /* +26 */
    ULONG64 frequency;   /* +32 QueryPerformanceFrequency's answer */
    ULONG64 tsc_sample;  /* +40 written only while disarmed */
};

C_ASSERT( sizeof(struct emu_qpc_guest) == 48 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, multiplier) == 8 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, bias)       == 16 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, enabled)    == 24 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, shift)      == 25 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, frequency)  == 32 );
C_ASSERT( FIELD_OFFSET(struct emu_qpc_guest, tsc_sample) == 40 );

#define EMU_QPC_GUEST_MULTIPLIER   8
#define EMU_QPC_GUEST_BIAS         16
#define EMU_QPC_GUEST_ENABLED      24
#define EMU_QPC_GUEST_SHIFT        25
#define EMU_QPC_GUEST_FREQUENCY    32
#define EMU_QPC_GUEST_TSC_SAMPLE   40

/* The largest TSC scale the arming code will accept.  fastppcx86 shifts until
 * the guest-visible rate clears 1 GHz (TSC_SCALE_MAXIMUM), so a 512 MHz
 * timebase gives 1 and even a 1 MHz one would give 10.  Anything past this is
 * not a machine this reasoning was checked against, and the bypass stays off
 * rather than guessing. */
#define EMU_QPC_MAX_SHIFT  16

#ifdef __powerpc64__

/* mftb: SPR 268, the 64-bit timebase.  Free-running, firmware-synchronised
 * across every core on the machine, and the clocksource Linux itself reads. */
static inline ULONG64 emu_qpc_timebase(void)
{
    ULONG64 tb;
    __asm__ __volatile__( "mfspr %0, 268" : "=r"(tb) );
    return tb;
}

static inline ULONG64 emu_qpc_mulhi( ULONG64 a, ULONG64 b )
{
    return (ULONG64)(((unsigned __int128)a * b) >> 64);
}

/* The native answer.  One multiply, one add. */
static inline ULONG64 emu_qpc_native( const struct emu_qpc_session *s )
{
    return emu_qpc_mulhi( emu_qpc_timebase(), s->multiplier ) + s->bias;
}

/* Valid only once the session block has been seeded; every caller checks. */
static inline BOOL emu_qpc_session_ok( const struct emu_qpc_session *s )
{
    return s->magic == EMU_QPC_MAGIC && s->multiplier != 0 && s->qpc_freq != 0;
}

#endif /* __powerpc64__ */

#endif /* __WINE_EMU_QPC_H */
