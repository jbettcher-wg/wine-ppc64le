#!/usr/bin/env bash
# Does a built artifact contain instructions a POWER8 cannot execute?
#
#   ./scan-isa.sh [dir-or-file ...]     # default: build-native
#   ./scan-isa.sh --self-test           # run only the built-in control, then stop
#
# Exit 0 clean, 1 if anything post-ISA-2.07 is found, 2 if the scanner itself
# could not be shown to work.
#
# ---------------------------------------------------------------------------
# THE ORACLE, AND WHY IT IS NOT THE ASSEMBLER
#
# The obvious oracle is to feed each instruction back through `as -mpower8` and
# treat a rejection as a finding. That is what this script used to do, and it
# was wrong twice over. Both were measured on the AC922, binutils 2.46.1:
#
#   1. `as` does not accept objdump's register spelling without -mregnames.
#      `add r3,r4,r5` is an error ("unsupported relocation against r3"), at
#      EVERY -mcpu level. Since the old script only reported an instruction
#      that power8 rejected AND power9 accepted, every register-bearing
#      instruction failed both probes, landed in the "unparsed, not a finding"
#      bucket, and was discarded. That is nearly the whole instruction set.
#
#   2. Even with -mregnames the assembler is not a level oracle, because it
#      silently substitutes legacy encodings. `as -mpower8 -mregnames` ACCEPTS
#      `lxvx vs34,r3,r4` -- and emits 0x7c432699, which is `lxvd2x`. The real
#      lxvx is 0x7c432219. An ISA 3.0 lxvx in a real object would have been
#      passed as clean.
#
# So the oracle is the DISASSEMBLER, applied to the bytes that are actually in
# the object. `objdump -Mpower8` decodes only what ISA 2.07 defines and prints
# `.long 0x...` for everything else; `objdump -Mpower10` names it. A word that
# is `.long` at power8 and a named instruction at power10 is, by construction,
# an encoding a POWER8 cannot execute. No spelling, no round-trip, no operand
# syntax involved -- and it covers ISA 3.1 as well as 3.0.
#
# DATA IN EXECUTABLE SECTIONS. `objdump -d` decodes every word of a code
# section, and GCC puts switch tables there: on ppc64le the offsets follow the
# `bctr` inline. Those are 32-bit data words, and one of them in this project's
# own build is 0xffffcd48, which decodes at power10 as `xsmaxcqp`. Reporting it
# would be a false alarm, and a scan that cries wolf gets switched off.
#
# The rule, and it is the one soft edge in this script: a run of words that
# decode only above the floor is treated as data if the word immediately before
# it AND the word immediately after it decode at NO ISA level. Compilers do not
# emit undecodable words next to real instructions, so this cannot fire in the
# middle of code; it fires inside a table, where the neighbouring offsets are
# not valid encodings either. Where it is unsure it keeps the finding, so its
# errors are false alarms rather than silence. Suppressed runs are counted and
# located in the output, never dropped quietly, and SCAN_NO_DATA_HEURISTIC=1
# turns the rule off entirely.
#
# It can still be fooled: a switch table whose every word happens to be a valid
# post-2.07 encoding would be reported (harmless), and a genuine ISA 3.0
# instruction sandwiched between two undecodable words would be missed. The
# latter needs the compiler to interleave code and garbage, which it does not.
#
# SELF-TEST. Run unconditionally before every scan, because the failure this
# script exists to prevent is a scan that reports CLEAN while testing nothing.
# Three control objects are assembled and scanned through the same code path;
# if any assertion fails the scan is abandoned with exit 2 rather than printing
# a result nobody should believe. See self_test() below for what they assert.
#
# CAVEAT on a clean result: this scans OUR objects. Instructions also arrive
# through statically linked libraries, and on a distro built -mcpu=power9 (as
# this machine is) libstdc++.a alone carries hundreds. Clean here means our
# code is clean, not that a statically linked binary will run on a POWER8.
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE_CPU="${SCAN_BASE_CPU:-power8}"   # the floor we are asserting
TOP_CPU="${SCAN_TOP_CPU:-power10}"    # used only to give findings a name

command -v objdump >/dev/null || { echo "scan-isa: objdump not found (binutils)" >&2; exit 2; }
command -v as      >/dev/null || { echo "scan-isa: as not found (binutils); the self-test needs it" >&2; exit 2; }

# --------------------------------------------------------------------------
# Core. Emits one line per finding: <object>|<section>|<addr>|<insn-at-TOP>,
# and the data-word counts to $SIDE_FILE.
scan() {
    local dump_base dump_top
    dump_base="$(mktemp)"; dump_top="$(mktemp)"

    # One objdump per level over every object: file and section headers in the
    # stream tell us which object each address belongs to.
    local normalise='
        /: +file format /      { f=$1; sub(/:$/,"",f); next }
        /^Disassembly of sect/ { s=$4; sub(/:$/,"",s);  next }
        /^[ \t]*[0-9a-f]+:\t/  {
            line=$0
            sub(/^[ \t]*/,"",line)
            i=index(line,":")
            addr=substr(line,1,i-1)
            insn=substr(line,i+2)               # past the tab
            sub(/[ \t]+$/,"",insn)
            print f "|" s "|" addr "\t" insn
        }'

    printf '%s\n' "$@" | xargs -r -d '\n' objdump -d --no-show-raw-insn -M"$BASE_CPU" 2>/dev/null \
        | awk "$normalise" > "$dump_base"
    printf '%s\n' "$@" | xargs -r -d '\n' objdump -d --no-show-raw-insn -M"$TOP_CPU"  2>/dev/null \
        | awk "$normalise" > "$dump_top"

    # Zero instructions out of a non-empty target list means objdump rejected
    # every one of them -- a directory of the wrong thing, a binutils that does
    # not know -M$TOP_CPU. Reporting that as "no violations found" is precisely
    # the failure this script exists to avoid.
    if [ ! -s "$dump_base" ] || [ ! -s "$dump_top" ]; then
        echo "scan-isa: objdump produced no instructions for these targets." >&2
        echo "          Either they are not ppc64le objects, or this binutils" >&2
        echo "          does not support -M$BASE_CPU / -M$TOP_CPU. Not scanning." >&2
        rm -f "$dump_base" "$dump_top"
        exit 2
    fi

    # Classify every word, in address order, as
    #   V  decodes at the floor            -> ordinary instruction
    #   X  decodes only above the floor    -> candidate finding
    #   D  decodes at no ISA level         -> cannot be an instruction at all
    # then apply the embedded-data rule described in the header comment.
    awk -F'\t' -v sidefile="$SIDE_FILE" -v keepdata="${SCAN_NO_DATA_HEURISTIC:-0}" '
        function flush(nextcls,   i) {
            if (n == 0) return
            # A run of X bounded on BOTH sides by a word that decodes at no ISA
            # level is inside embedded data, not code.
            if (keepdata != 1 && runprev == "D" && nextcls == "D") {
                supp += n
                if (suppwhere == "") suppwhere = run[1]
                suppruns++
            } else {
                for (i = 1; i <= n; i++) print run[i]
            }
            n = 0
        }
        NR == FNR { base[$1] = $2; next }
        {
            k = $1
            j = k; sub(/\|[^|]*$/, "", j)        # file|section, address dropped
            if (j != prevfs) { flush("B"); prevcls = "B"; prevfs = j }

            b   = (k in base) ? base[k] : ""
            cls = (b !~ /^\.long/) ? "V" : (($2 ~ /^\.long/) ? "D" : "X")
            if (cls == "D") undec++

            if (cls == "X") {
                if (n == 0) runprev = prevcls
                run[++n] = k "|" $2
            } else {
                flush(cls)
            }
            prevcls = cls
        }
        END { flush("B"); printf "%d %d %d %s\n", undec+0, supp+0, suppruns+0, suppwhere > sidefile }
    ' "$dump_base" "$dump_top"

    rm -f "$dump_base" "$dump_top"
}

# --------------------------------------------------------------------------
# Self-test. Three assertions, run before every scan:
#
#   A  five ISA 3.0 encodings sitting in ordinary code are all reported
#      -- the scan can fail. This is the one that matters; the previous
#      assembler-based scan would have found 1 of these 5.
#   B  one ISA 3.0 encoding walled in by words no ISA level can decode is
#      suppressed -- the embedded-data rule is in force.
#   C  ...and B is reported when the rule is switched off, so B passing means
#      "suppressed", not "never detected in the first place".
#
# B and C together pin the data rule from both sides. Without C, a scan that
# had stopped detecting anything at all would still pass B.
fail() { echo "scan-isa: SELF-TEST FAILED -- $*" >&2
         echo "          The scanner is broken; a clean result from it would mean" >&2
         echo "          nothing. Refusing to scan." >&2; }

self_test() {
    local d a b c
    d="$(mktemp -d)"
    SIDE_FILE="$d/side"

    cat > "$d/code.s" <<'EOF'
	.text
	.globl isa30_in_code
isa30_in_code:
	darn 3,1
	mcrxrx 0
	cnttzd 4,5
	xxbrd 32,33
	lxvx 34,3,4
	blr
EOF
    # 0xe778 is a word from a real GCC jump table in this project's own build:
    # it decodes at no ISA level, which is what makes its neighbours data.
    cat > "$d/data.s" <<'EOF'
	.text
	.globl isa30_in_data
isa30_in_data:
	blr
	.long 0xe778
	darn 3,1
	.long 0xe778
EOF
    if ! as -mpower9 -o "$d/code.o" "$d/code.s" 2>"$d/err" \
    || ! as -mpower9 -o "$d/data.o" "$d/data.s" 2>>"$d/err"; then
        fail "the control objects could not be assembled"
        sed 's/^/    /' "$d/err" >&2
        rm -rf "$d"; return 2
    fi

    a="$(scan "$d/code.o" | wc -l)"
    b="$(scan "$d/data.o" | wc -l)"
    c="$(SCAN_NO_DATA_HEURISTIC=1; scan "$d/data.o" | wc -l)"
    rm -rf "$d"

    [ "$a" -eq 5 ] || { fail "control A: 5 ISA 3.0 instructions in code, $a reported"; return 2; }
    [ "$b" -eq 0 ] || { fail "control B: ISA 3.0 word inside data, $b reported (want 0)"; return 2; }
    [ "$c" -eq 1 ] || { fail "control C: same word with the data rule off, $c reported (want 1)"; return 2; }
    return 0
}

# --------------------------------------------------------------------------
selftest_only=0
[ "${1:-}" = "--self-test" ] && { selftest_only=1; shift; }

if self_test; then
    echo "  self-test: A 5/5 detected in code, B suppressed inside data, C 1/1 with the rule off"
else
    exit 2
fi
[ "$selftest_only" = 1 ] && exit 0

targets=("${@:-$HERE/build-native}")

objs=()
for t in "${targets[@]}"; do
    if [ -d "$t" ]; then
        # Prefer .o over the linked .so: a linked library drags in whatever the
        # toolchain statically added, which is not ours to answer for.
        while IFS= read -r f; do objs+=("$f"); done < <(find "$t" -name '*.o' -type f | sort)
    elif [ -f "$t" ]; then
        objs+=("$t")
    else
        echo "scan-isa: no such path: $t" >&2; exit 2
    fi
done
[ ${#objs[@]} -eq 0 ] && { echo "scan-isa: nothing to scan under ${targets[*]}" >&2; exit 2; }

echo "  scanning ${#objs[@]} object(s) against the $BASE_CPU floor"

SIDE_FILE="$(mktemp)"
findings="$(scan "${objs[@]}")"
read -r undec supp suppruns suppwhere < "$SIDE_FILE"; rm -f "$SIDE_FILE"

echo "  $undec word(s) decode at no ISA level: alignment padding, literal pools"
echo "    and switch tables the compiler placed in an executable section."
if [ "${supp:-0}" -gt 0 ]; then
    echo "  $supp word(s) in $suppruns run(s) decode above the floor but are walled in"
    echo "    by those, so they are data, not instructions. Not reported below."
    echo "    First: ${suppwhere%%|*} $(echo "$suppwhere" | cut -d'|' -f2-3 | tr '|' '+')"
    echo "    Re-run with SCAN_NO_DATA_HEURISTIC=1 to see them."
fi

# Group by mnemonic, not by address: one bad inline function expanded 400 times
# is one problem, and a 400-line report is one nobody reads.
if [ -n "$findings" ]; then
    echo
    echo "  ${BASE_CPU^^} FLOOR VIOLATED"
    printf '%s\n' "$findings" \
        | awk -F'|' '{ split($4,a,/[ \t]/); m=a[1]; n[m]++; if (!(m in where)) where[m]=$1 " (" $2 "+" $3 ")" }
                     END { for (m in n) printf "    %-14s %6d site(s), first at %s\n", m, n[m], where[m] }' \
        | sort
    echo
    echo "  $(printf '%s\n' "$findings" | wc -l) instruction word(s) total."
    echo "  Rebuild with PPC_MCPU=$BASE_CPU, or record why this artifact is"
    echo "  deliberately POWER9. Do not silence this by editing the scan."
    exit 1
fi

echo "  CLEAN -- no word in these objects decodes above the $BASE_CPU floor"
