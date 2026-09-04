#!/bin/sh
#
# check-lean-return-fpvr.sh -- the lean trap return restores no FPR/VR, and
# the one PE frame still live when it runs never touches them.
#
# emu_trap_return_direct (dlls/ntdll/unix/signal_ppc64.c) is reached by an
# ordinary CALL from the PE dispatcher at the normal end of a dispatch.  Every
# callee between call_user_mode_callback's entry and that call has returned,
# so by the ELFv2 contract f14-f31 and v20-v31 already hold their entry
# values -- PROVIDED the frames still live at the call do not use them.  Those
# frames are exactly the two PE functions that call the stub:
# emu_trap_dispatch and emu_exception_dispatch.  This gate reads the built
# PE object and fails the moment either grows a non-volatile FPR/VR use
# (a compiler upgrade vectorising a struct copy is the realistic way), which
# is the day the reload must come back.
#
# Layers:
#   1. the two PE frames use no f14-f31 / v20-v31 (vs52-vs63)
#   2. the lean stub carries no FPR/VR loads; the syscall-route return
#      (user_mode_callback_return) still carries all 30 -- the removal is
#      scoped to the path whose guards make it sound
#   3. detector liveness: the same pattern DOES find call_user_mode_callback's
#      18 stfd + 12 stvx, so an empty match in layer 1 is evidence, not a
#      broken regex
#
# Exit 0 = pass, 1 = fail, 2 = could not run.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}
fail=0

bad()  { echo "check-lean-return-fpvr: FAIL $*" >&2; fail=1; }
skip() { echo "check-lean-return-fpvr: $*" >&2; exit 2; }

PEOBJ=$BUILD/dlls/ntdll/signal_ppc64.o
UNIXSO=$BUILD/dlls/ntdll/ntdll.so
[ -f "$PEOBJ" ]  || skip "no PE object at $PEOBJ (build dlls/ntdll first)"
[ -f "$UNIXSO" ] || skip "no unix library at $UNIXSO"
if command -v llvm-objdump >/dev/null; then OBJDUMP=llvm-objdump
elif command -v objdump >/dev/null; then OBJDUMP=objdump
else skip "need llvm-objdump or objdump"; fi

# instruction lines only, whitespace normalised
disas() { $OBJDUMP -d --disassemble-symbols="$2" "$1" 2>/dev/null | grep -E '^ *[0-9a-f]+:' | sed 's/  */ /g'; }

# non-volatile FPR: any FP load/store/move naming f14..f31 as its register
FPR_NV='\b(stfd|lfd|stfs|lfs|stfdu|lfdu|stfdx|lfdx|fmr)[. ]+(1[4-9]|2[0-9]|3[01])\b'
# non-volatile VR/VSR: v20..v31 as the VMX register, vs52..vs63 as the VSX alias
VR_NV='\b(stvx|lvx|stvxl|lvxl|vor)[. ]+(2[0-9]|3[01])\b|\b(stxvd2x|lxvd2x|stxvw4x|lxvw4x|stxv|lxv|xxlor)[. ]+(5[2-9]|6[0-3])\b'

# ---- layer 1: the live PE frames ----
for f in emu_trap_dispatch emu_exception_dispatch; do
    d=$(disas "$PEOBJ" "$f")
    n=$(printf '%s\n' "$d" | grep -c .)
    [ "$n" -gt 50 ] || { bad "$f: only $n instructions disassembled from $PEOBJ -- symbol missing?"; continue; }
    fp=$(printf '%s\n' "$d" | grep -cE "$FPR_NV")
    vr=$(printf '%s\n' "$d" | grep -cE "$VR_NV")
    if [ "$fp" -ne 0 ] || [ "$vr" -ne 0 ]; then
        bad "$f uses non-volatile FPR/VR ($fp FPR, $vr VR sites) -- the lean return must reload again"
        printf '%s\n' "$d" | grep -E "$FPR_NV|$VR_NV" | head -5 >&2
    else
        echo "check-lean-return-fpvr: $f: $n insns, no f14-f31 / v20-v31 use"
    fi
done

# ---- layer 2: the stub is bare, the syscall route is not ----
lean=$(disas "$UNIXSO" emu_trap_return_direct | grep -cE '\b(lfd|lvx|lxv|lxvd2x)\b')
sysr=$(disas "$UNIXSO" user_mode_callback_return | grep -cE '\b(lfd|lvx|lxv|lxvd2x)\b')
[ "$lean" -eq 0 ]  || bad "emu_trap_return_direct still carries $lean FPR/VR loads"
[ "$sysr" -ge 30 ] || bad "user_mode_callback_return carries only $sysr FPR/VR loads (expected 30) -- the removal leaked into the syscall route"
[ "$fail" -eq 0 ] && echo "check-lean-return-fpvr: lean stub 0 FPR/VR loads, syscall route $sysr"

# ---- layer 3: detector liveness ----
live_fp=$(disas "$UNIXSO" call_user_mode_callback | grep -cE "$FPR_NV")
live_vr=$(disas "$UNIXSO" call_user_mode_callback | grep -cE "$VR_NV")
[ "$live_fp" -eq 18 ] && [ "$live_vr" -eq 12 ] \
    || bad "detector liveness: call_user_mode_callback shows $live_fp FPR / $live_vr VR non-volatile sites, expected 18 / 12"
[ "$fail" -eq 0 ] && echo "check-lean-return-fpvr: detector sees call_user_mode_callback's 18 + 12 saves"

[ "$fail" -eq 0 ] && echo "check-lean-return-fpvr: PASS"
exit $fail
