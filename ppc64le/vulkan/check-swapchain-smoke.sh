#!/bin/sh
#
# check-swapchain-smoke.sh -- the native-vs-guest Vulkan WSI RUNTIME gate.
#
# The claim: an x86-64 Windows program run as a GUEST under this port creates
# a Win32 Vulkan surface on a real HWND, builds a swapchain on it, asks
# vkGetSwapchainImagesKHR for that swapchain's images, clears one, reads it
# back TEXEL-EXACT and presents it -- and gets BYTE-IDENTICAL output to the
# same source built for this machine's OWN architecture and run through the
# same wine, the same win32u WSI path and the same driver.  Only the caller's
# instruction set differs, and therefore only whether every one of those calls
# crossed the guest thunk boundary.
#
# WHY IT EXISTS, AND WHY IT DID NOT
#
# A guest game called vkGetSwapchainImagesKHR through the vulkan-1 guest
# thunk, was told VK_SUCCESS, and died on the value.  Nothing in this port had
# ever made that call from a Win32 client.  The D3D12 and D3D11 legs present
# through __wine_get_hwnd_surface_funcs on their OWN foreign VkInstance (see
# include/wine/vulkan_driver.h), which never touches win32u's client-object
# wrapping; ppc64le/dxvk/check-d3d11-smoke.sh runs headless with
# DXVK_WSI_DRIVER=Headless and creates no swapchain at all.  So the whole
# vulkan-1 -> winevulkan -> win32u WSI path was ungated, and a game found it
# first.  That is the gap this closes.
#
# Legs:
#
#   A  BUILD: the guest vulkan-1 thunk exists and exports the WSI surface --
#      a guest that imports vulkan-1 and reaches vkCreateSwapchainKHR through
#      a hole in the export table gets ntdll's missing-import sentinel, which
#      is a different bug wearing the same coat.
#   B  DISPLAY: a compositor of this gate's own.  Vulkan WSI needs a
#      presentable surface, and Xvfb cannot give one: it has no DRI3, so RADV
#      reports no presentation support and vkCreateSwapchainKHR never happens
#      (measured -- "Presentable Surfaces:" comes back empty on an Xvfb).
#      A headless weston with the GL renderer does have one, on the real GPU,
#      and takes no display anyone is using.  The person at this machine is on
#      their own session and this gate never looks at it.
#
#      The legs run through weston's own Xwayland, i.e. winex11, the driver
#      every other graphics gate here uses.  ONE transport per run and not two:
#      Wine settles on a display driver once per PREFIX SESSION, so a second
#      leg with the other transport in the same prefix gets a NULL HWND and a
#      surface-lost swapchain -- measured, and an artefact of the gate rather
#      than of the port.  The winewayland transport (which is where the game
#      landed: the environment Steam handed it has no DISPLAY at all) was
#      measured by hand against both a headless weston and a headless KWin and
#      behaves identically leg-for-leg, including on leg F below.
#   C  NATIVE: the probe built as a ppc64 PE (winegcc), run under this wine
#      with no guest anywhere in the process -- once per transport.
#   D  GUEST: the same source built as an x86-64 PE, run as a guest -- once
#      per transport.
#   E  IDENTITY: cmp(native stdout, guest stdout) is empty, per transport.
#   F  VALUES: the transcript is read, not just its verdict line -- the image
#      count, the non-null and distinct counts, the sentinel count, the texel
#      coverage and the mismatch count are each checked here as well.  A probe
#      that stopped printing a field, or printed "checked=1", cannot pass this
#      leg by having said ok to itself.
#   G  BOTH LEGS PASS.
#
# --sabotage runs the negative controls instead and requires every one to go
# red.  A gate that cannot go red proves nothing.  Each is a VK_SWAPCHAIN_BREAK
# build of the NATIVE leg -- native, so that a red is unambiguously the check
# and not the boundary:
#
#   1  no clear: the readback sees whatever was in a fresh swapchain image.
#   2  the expectation's R and B swapped: the bytes are right, the check is
#      deliberately wrong, and all 55056 texels must mismatch.
#   3  one texel checked instead of all: coverage is part of the claim, so the
#      verdict fails on arithmetic rather than by luck.
#   4  the second vkGetSwapchainImagesKHR skipped entirely, leaving the array
#      full of sentinels.  This is the GAME'S OWN failure mode -- VK_SUCCESS
#      and no usable images -- and the gate must see it.
#   5  one handle of the repeated fetch corrupted, so the stability check must
#      go red.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run at all (a skip is NOT
# a pass).
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-gl-smoke.sh because
# the hazard is identical: the bringup prefix has AeDebug configured with
# "winedbg --auto", so a run that ends in an unhandled fault -- which is what a
# defect here looks like from outside -- starts a debugger that attaches and
# never lets go, turning every red state of this gate into a hang.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/vk-swapchain-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-swapchain-smoke: $*"; }
bad()  { echo "check-swapchain-smoke: FAIL $*" >&2; fail=1; }
note() { echo "check-swapchain-smoke: note $*"; }
skip() { echo "check-swapchain-smoke: $*" >&2; cleanup; exit 2; }

WESTON_PID=
WL_RUNTIME=
cleanup() {
    [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
    WESTON_PID=
}
trap 'cleanup' EXIT INT TERM

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v "${CC:-gcc}" >/dev/null || skip "need ${CC:-gcc} for the native ppc64 build"
command -v weston >/dev/null || skip "need weston: a Vulkan swapchain needs a \
presentable surface, an Xvfb has no DRI3 and cannot give one, and this gate \
must not take the display the user is on"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
GUEST_THUNK="$BUILD/dlls/vulkan-1/x86_64-windows/vulkan-1.dll"

# ---- A: the guest thunk exists and carries the WSI surface -----------------
[ -f "$GUEST_THUNK" ] || skip "no guest vulkan-1 thunk at $GUEST_THUNK; build it first"
MISSING=$(python3 - "$GUEST_THUNK" <<'EOF'
import struct, sys
want = ["vkCreateWin32SurfaceKHR", "vkCreateSwapchainKHR", "vkGetSwapchainImagesKHR",
        "vkAcquireNextImageKHR", "vkQueuePresentKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"]
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
edir_rva = struct.unpack_from('<I', d, pe + 24 + 112)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    va, rs, pr = struct.unpack_from('<III', d, o + 12)
    secs.append((va, rs, pr))
def off(rva):
    for va, rs, pr in secs:
        if va <= rva < va + rs:
            return pr + rva - va
    return None
e = off(edir_rva)
nnames = struct.unpack_from('<I', d, e + 24)[0]
names_rva = struct.unpack_from('<I', d, e + 32)[0]
have = set()
for i in range(nnames):
    r = struct.unpack_from('<I', d, off(names_rva) + 4 * i)[0]
    o = off(r)
    have.add(d[o:d.index(b'\0', o)].decode('latin1'))
print(" ".join(n for n in want if n not in have), end="")
EOF
) || skip "cannot read the export table of $GUEST_THUNK"
if [ -n "$MISSING" ]; then
    bad "the guest vulkan-1 thunk does not export: $MISSING -- a guest that \
imports these binds ntdll's missing-import sentinel instead"
else
    say "thunk: $GUEST_THUNK exports the whole WSI surface the probe imports"
fi

# ---- build: the native ppc64 PE leg ----------------------------------------
# Built exactly the way ppc64le/opengl/check-gl-smoke.sh builds its native
# lane: an ordinary consumer of the public headers, linked against this tree's
# own ppc64-windows import libraries and turned into a builtin PE.  It runs
# under the SAME wine as the guest leg, so both reach the same vulkan-1.
native_build() {   # native_build <output> [extra cflags...]
    nout=$1; shift
    ${CC:-gcc} -c -o "$OUT/native.o" "$HERE/probes/vk_swapchain_smoke.c" $INCL \
        -DVK_SMOKE_NATIVE "$@" \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$nout" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/native.o" \
        "$BUILD/dlls/vulkan-1/ppc64-windows/libvulkan-1.a" \
        "$BUILD/dlls/user32/ppc64-windows/libuser32.a" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ucrtbase/ppc64-windows/libucrtbase.a" \
        "$BUILD/dlls/kernel32/ppc64-windows/libkernel32.a" \
        "$BUILD/dlls/ntdll/ppc64-windows/libntdll.a" \
        2>>"$OUT/native.build.err" || return 1
    rm -f "$nout"
    "$SRC/tools/elf2pe" "$nout.so" "$nout" 2>>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winebuild/winebuild" --builtin "$nout" \
        2>>"$OUT/native.build.err" || return 1
    return 0
}

# ---- build: the x86-64 guest PE leg ----------------------------------------
# The same clang x86_64-windows-gnu machinery check-gl-smoke.sh drives its
# guest build with and the same Wine headers, so any disagreement between the
# two legs is the boundary and not the declarations.  The imports are named by
# hand: the guest binds to the same builtins a real application would and
# there is no CRT here at all, see the probe's header.
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetModuleHandleA
EOF
cat > "$OUT/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
RegisterClassA
CreateWindowExA
DefWindowProcA
DestroyWindow
ShowWindow
GetClientRect
EOF
cat > "$OUT/vulkan-1.def" <<'EOF'
LIBRARY vulkan-1.dll
EXPORTS
vkCreateInstance
vkDestroyInstance
vkEnumeratePhysicalDevices
vkGetPhysicalDeviceProperties
vkGetPhysicalDeviceQueueFamilyProperties
vkGetPhysicalDeviceMemoryProperties
vkEnumerateDeviceExtensionProperties
vkCreateWin32SurfaceKHR
vkDestroySurfaceKHR
vkGetPhysicalDeviceSurfaceSupportKHR
vkGetPhysicalDeviceSurfaceCapabilitiesKHR
vkGetPhysicalDeviceSurfaceFormatsKHR
vkGetPhysicalDeviceSurfacePresentModesKHR
vkCreateDevice
vkDestroyDevice
vkGetDeviceQueue
vkCreateSwapchainKHR
vkDestroySwapchainKHR
vkGetSwapchainImagesKHR
vkAcquireNextImageKHR
vkQueuePresentKHR
vkCreateCommandPool
vkDestroyCommandPool
vkAllocateCommandBuffers
vkBeginCommandBuffer
vkEndCommandBuffer
vkCmdPipelineBarrier
vkCmdClearColorImage
vkCmdCopyImageToBuffer
vkQueueSubmit
vkCreateFence
vkDestroyFence
vkWaitForFences
vkResetFences
vkCreateBuffer
vkDestroyBuffer
vkGetBufferMemoryRequirements
vkAllocateMemory
vkFreeMemory
vkBindBufferMemory
vkMapMemory
vkUnmapMemory
vkDeviceWaitIdle
EOF
for m in kernel32 user32 vulkan-1; do
    llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
        || skip "llvm-dlltool failed for $m"
done

GUESTCC="clang -target x86_64-windows-gnu -nostdlibinc $INCL -Wall -O1 -fno-builtin -g"
GUESTLD="clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
-Wl,--entry=vk_swapchain_smoke_entry -Wl,--subsystem,console"

guest_build() {   # guest_build <output> [extra cflags...]
    gout=$1; shift
    $GUESTCC "$@" -c -o "$OUT/guest.o" "$HERE/probes/vk_swapchain_smoke.c" \
        2>"$OUT/guest.build.err" || return 1
    $GUESTLD -o "$gout" "$OUT/guest.o" "$OUT/libvulkan-1.a" "$OUT/libuser32.a" \
        "$OUT/libkernel32.a" 2>>"$OUT/guest.build.err" || return 1
    return 0
}

native_build "$OUT/native.exe" || {
    tail -20 "$OUT/native.build.err" >&2
    skip "the native ppc64 build failed"
}
guest_build "$OUT/guest.exe" || {
    tail -20 "$OUT/guest.build.err" >&2
    skip "the x86-64 guest build failed"
}
say "build: both legs built from $HERE/probes/vk_swapchain_smoke.c"

# ---- B: a compositor of this gate's own ------------------------------------
# NEVER the session someone is on.  A headless weston with the GL renderer
# gives a real presentable Vulkan surface on the real GPU and appears on
# nobody's screen; its socket name and XDG_RUNTIME_DIR are this gate's own, so
# a client of ours cannot reach the user's compositor by accident and a client
# of theirs cannot reach ours.
WL_RUNTIME="$OUT/runtime"
WL_SOCKET="vk-swapchain-smoke-$$"
rm -rf "$WL_RUNTIME"
mkdir -p "$WL_RUNTIME" || skip "cannot create $WL_RUNTIME"
chmod 700 "$WL_RUNTIME"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$WL_RUNTIME" \
    weston --backend=headless --renderer=gl --xwayland --socket="$WL_SOCKET" \
           --width=640 --height=480 --idle-time=0 \
    > "$OUT/weston.log" 2>&1 &
WESTON_PID=$!
n=0
while [ ! -S "$WL_RUNTIME/$WL_SOCKET" ]; do
    n=$((n + 1))
    [ "$n" -gt 200 ] && break
    sleep 0.1
done
if [ ! -S "$WL_RUNTIME/$WL_SOCKET" ]; then
    sed 's/^/  weston| /' "$OUT/weston.log" >&2
    skip "weston did not come up on $WL_RUNTIME/$WL_SOCKET"
fi
grep -q "Using GL renderer" "$OUT/weston.log" || skip "weston came up without \
the GL renderer; its surfaces would not be presentable on this GPU"
# Weston's own Xwayland, so the gate covers BOTH of the port's display drivers:
# winewayland, which is where the game landed (no DISPLAY in the environment
# Steam handed it), and winex11, which is what every other graphics gate here
# uses.  They reach win32u's WSI by different routes and report different
# surface capabilities, and the claim has to hold on both.  Weston takes the
# first FREE display number, never one somebody is on.
n=0
while ! grep -q "xserver listening on display" "$OUT/weston.log"; do
    n=$((n + 1))
    [ "$n" -gt 200 ] && break
    sleep 0.1
done
X_DISPLAY=$(sed -n 's/.*xserver listening on display \(:[0-9]*\).*/\1/p' \
            "$OUT/weston.log" | head -1)
[ -n "$X_DISPLAY" ] || skip "weston brought up no Xwayland server"
say "display: headless weston (pid $WESTON_PID), wayland socket $WL_SOCKET and \
Xwayland $X_DISPLAY; the user's own session is untouched"

WDBG=${WINEDEBUG:--all}
run_wine() {   # run_wine <transport> <exe> <stdout> <stderr>
    tr=$1; exe=$2; sout=$3; serr=$4
    if [ "$tr" = wayland ]; then
        timeout -k 5 "$TIMEOUT" env -u DISPLAY WAYLAND_DISPLAY="$WL_SOCKET" \
            XDG_RUNTIME_DIR="$WL_RUNTIME" WINEDEBUG="$WDBG" \
            WINEDLLOVERRIDES="winedbg.exe=d" \
            "$BUILD/wine" "$exe" > "$sout" 2> "$serr"
    else
        timeout -k 5 "$TIMEOUT" env -u WAYLAND_DISPLAY DISPLAY="$X_DISPLAY" \
            XDG_RUNTIME_DIR="$WL_RUNTIME" WINEDEBUG="$WDBG" \
            WINEDLLOVERRIDES="winedbg.exe=d" \
            "$BUILD/wine" "$exe" > "$sout" 2> "$serr"
    fi
}

# ---- (also standalone as --sabotage): the negative controls -----------------
sabotage() {
    ok=1
    for b in 1 2 3 4 5; do
        native_build "$OUT/sab$b.exe" "-DVK_SWAPCHAIN_BREAK=$b" || {
            echo "check-swapchain-smoke: FAIL VK_SWAPCHAIN_BREAK=$b did not \
build" >&2
            tail -10 "$OUT/native.build.err" >&2
            ok=0
            continue
        }
        run_wine x11 "$OUT/sab$b.exe" "$OUT/sab$b.out" "$OUT/sab$b.err"
        if grep -q "vk_swapchain_smoke: PASS" "$OUT/sab$b.out"; then
            echo "check-swapchain-smoke: FAIL VK_SWAPCHAIN_BREAK=$b still \
PASSED -- the gate cannot go red" >&2
            ok=0
        else
            echo "check-swapchain-smoke: sabotage $b red: \
$(grep -m1 'vk_swapchain_smoke:' "$OUT/sab$b.out")"
        fi
    done
    return $((1 - ok))
}

if [ "$SABOTAGE" = 1 ]; then
    if sabotage; then
        say "SABOTAGE OK -- every negative control went red"
        cleanup
        exit 0
    fi
    echo "check-swapchain-smoke: SABOTAGE FAILED" >&2
    cleanup
    exit 1
fi

# ---- F: the values, read here rather than taken on the probe's word ---------
# The probe says ok to itself; this reads the numbers out of the transcript so
# that a probe which stopped printing a field, or checked one texel, cannot
# reach a PASS through this gate.
field() {   # field <transport> <label> <sed-capture>
    v=$(sed -n "s/.*$3.*/\1/p" "$OUT/$1-guest.out" | head -1)
    [ -n "$v" ] || { bad "the $1 transcript carries no $2"; echo ""; return; }
    echo "$v"
}

check_values() {
    tr=$1
    IMG_COUNT=$(field "$tr" "image count"          'images-count: result=0 count=\([0-9]*\)')
    NONNULL=$(field   "$tr" "non-null image count" 'images-fetch: .* nonnull=\([0-9]*\)')
    SENTINELS=$(field "$tr" "sentinel count"       'images-fetch: .* sentinels=\([0-9]*\)')
    DISTINCT=$(field  "$tr" "distinct image count" 'images-fetch: .* distinct=\([0-9]*\)')
    SAME=$(field      "$tr" "stable image count"   'images-stable: .* same=\([0-9]*\)')
    TEXELS=$(field    "$tr" "texel total"          'readback: .* texels=\([0-9]*\)')
    CHECKED=$(field   "$tr" "texel coverage"       'readback: .* checked=\([0-9]*\)')
    MISMATCH=$(field  "$tr" "texel mismatches"     'readback: .* mismatches=\([0-9]*\)')

    [ "${IMG_COUNT:-0}" -gt 0 ] 2>/dev/null || \
        bad "$tr: vkGetSwapchainImagesKHR reported ${IMG_COUNT:-no} images"
    [ "${NONNULL:-0}" = "${IMG_COUNT:-x}" ] || \
        bad "$tr: $NONNULL of $IMG_COUNT swapchain images came back non-NULL"
    [ "${SENTINELS:-1}" = 0 ] || \
        bad "$tr: $SENTINELS swapchain image slots were never written -- \
VK_SUCCESS and an untouched array is exactly what the game saw"
    [ "${DISTINCT:-0}" = "${IMG_COUNT:-x}" ] || \
        bad "$tr: only $DISTINCT of $IMG_COUNT swapchain images are distinct"
    [ "${SAME:-0}" = "${IMG_COUNT:-x}" ] || \
        bad "$tr: two identical vkGetSwapchainImagesKHR calls answered \
differently ($SAME of $IMG_COUNT agreed)"
    [ "${CHECKED:-0}" = "${TEXELS:-x}" ] || \
        bad "$tr: the readback checked $CHECKED of $TEXELS texels"
    [ "${MISMATCH:-1}" = 0 ] || \
        bad "$tr: $MISMATCH of $TEXELS presented texels did not read back as \
cleared"
    say "$tr values: $IMG_COUNT images, all non-NULL, all distinct, stable \
across two queries; $CHECKED/$TEXELS texels exact"
}

# ---- C/D/E/F: the two legs, on each transport --------------------------------
for tr in x11; do
    run_wine "$tr" "$OUT/native.exe" "$OUT/$tr-native.out" "$OUT/$tr-native.err"
    run_wine "$tr" "$OUT/guest.exe"  "$OUT/$tr-guest.out"  "$OUT/$tr-guest.err"

    for leg in native guest; do
        if grep -q "vk_swapchain_smoke: PASS" "$OUT/$tr-$leg.out"; then
            say "$tr $leg: $(grep -m1 'vk_swapchain_smoke:' "$OUT/$tr-$leg.out")"
        else
            sed 's/^/  '"$tr $leg"'| /' "$OUT/$tr-$leg.out" >&2
            bad "the $tr $leg leg did not PASS"
        fi
    done

    if cmp -s "$OUT/$tr-native.out" "$OUT/$tr-guest.out"; then
        say "$tr identity: the two transcripts are byte-identical \
($(wc -c < "$OUT/$tr-native.out") bytes)"
    else
        diff -u "$OUT/$tr-native.out" "$OUT/$tr-guest.out" | head -40 >&2
        bad "the $tr guest transcript differs from the native one -- the thunk \
boundary changed a value"
    fi

    check_values "$tr"
done

# The port's own answers, from the guest leg only -- they are true of this
# machine and not of Vulkan, so they never enter the diffed transcript.
note "guest handles: $(tr '\n' ' ' < "$OUT/x11-guest.err" | sed 's/  */ /g' | cut -c1-200)"

if [ "$fail" = 0 ]; then
    say "PASS"
    cleanup
    exit 0
fi
say "FAILED" >&2
cleanup
exit 1
