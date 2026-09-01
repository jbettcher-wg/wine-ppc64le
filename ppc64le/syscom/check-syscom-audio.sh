#!/bin/sh
#
# check-syscom-audio.sh -- the CoCreateInstance AUDIO gate for the system-COM
# surface.
#
# THE CLAIM: an x86-64 Windows program run as a GUEST under this port asks
# combase for CLSID_XAudio2 (the 2.7 coclass, which is the only way a 2.7 title
# can reach the engine at all -- xaudio2_7.dll exports no creator) and for
# CLSID_MMDeviceEnumerator, walks both surfaces to something checkable, and
# gets BYTE-IDENTICAL output to the same source built for this machine's OWN
# architecture and run through the same wine, the same xaudio2_7, the same
# mmdevapi and the same host device.  Only the caller's instruction set
# differs -- and therefore only whether every one of those calls crossed the
# system-COM boundary.
#
# It is a DIFFERENT lane from ppc64le/audio/check-audio-smoke.sh, which must
# stay green alongside it: that one exercises XAudio2Create() and
# DirectSoundCreate(), flat exports of guest thunk modules with their own
# winecom instances and their own rosters.  Nothing here calls a flat export.
#
# Byte-identical is the bar rather than "the guest said PASS" for the reason
# check-com-smoke.sh gives: reaching the right answer through the wrong
# mechanism is exactly the failure a PASS/PASS comparison cannot see.  Every
# byte compared here is a value the implementation computed -- a voice's
# channel count and sample rate, the raw bits of two floats of which one
# travelled on the guest's stack, an endpoint's device state, a mix format, a
# one-second buffer's length in frames, an engine's reference count falling to
# zero.
#
# NOTHING IS PLAYED AT ANYONE.  This machine's sinks belong to the person using
# it, so the gate loads a null sink OF ITS OWN, points PULSE_SINK at it for the
# duration, and unloads it on the way out.  Wine's pulse driver puts the
# PULSE_SINK-following "PulseAudio Output" device first in its endpoint list, so
# the default endpoint and device index 0 -- which is what the probe opens --
# are that sink and nothing else.  The default sink is never changed and no
# existing sink is ever opened.
#
# Legs:
#
#   A  ROSTER CURRENT: gen_syscom_audio.py --roster --check proves the audio
#      family in interfaces_syscom.json is a byte-identical regeneration from
#      Wine's own headers (dlls/xaudio2_7/xaudio_classes.h -- the 2.7 widl run,
#      not include/xaudio2.h -- plus mmdeviceapi.h and audioclient.h);
#      --marshal --check proves dlls/combase/syscom_marshal.h matches that
#      roster; --selfcheck proves the parse/re-emit round trip that licenses
#      reusing the 58 interface blocks this generator did not write.
#   B  THUNKS: the three guest thunk modules that publish the system-COM stub
#      arrays -- combase, ole32, oleaut32 -- each publish the interface and slot
#      counts the marshal table expects.  Read out of the module's own
#      __wine_com_thunk_info the way libs/winecom reads it at attach.  This leg
#      exists because a roster change does NOT rebuild them: makedep's
#      output_source_thunks() lists the .thunks file, spec2thunk and wine_sig.py
#      as prerequisites and not the COM-JSON the .thunks file names.
#   C  SINK: a null sink of this gate's own.
#   D  NATIVE: the probe built as a ppc64 PE, run under this wine with no guest
#      anywhere in the process, reports PASS.
#   E  GUEST: the same source built as an x86-64 PE, run as a guest, PASS.
#   F  IDENTITY: cmp(native stdout, guest stdout) is empty.
#   G  REFUSALS AND SERVICES: the guest's stderr must show the two entry points
#      that are still refused answering E_NOTIMPL where the native leg succeeds
#      -- and the two that are now SERVED answering the SAME value the native
#      leg gets.  IXAudio2::RegisterForCallbacks and
#      IMMDeviceEnumerator::UnregisterEndpointNotificationCallback used to be
#      in the first list; the reverse direction is on for this surface now
#      (WINECOM_SF_REVERSE in dlls/combase/syscom.c) and both are served,
#      which is what leg I proves is real rather than merely non-failing.
#   H2 HAZARD: the 19 REUSED slots whose interface in-parameters this port
#      declines to reverse-proxy must still fail closed -- checked in the
#      shipped marshal header itself (their rows must stop at aux2, so there is
#      no xmask bit and libs/winecom reads no type it was never given) and
#      against gen_syscom_audio.py's own generated withheld list.  They are the
#      CA_IFACE_IN-bearing slots of the four legacy interfaces a guest
#      application really implements and hands to native COM: IMoniker,
#      IRunningObjectTable, IDirectMusicTool and IDirectMusicTrack.  This is
#      the single most important safety property of the flip: an xmask bit
#      there would let this surface build a native vtable around a guest object
#      whose own table refuses in both directions, and whose QueryInterface
#      could then mint one of any other type on the roster.
#   I  REVERSE: a GUEST-implemented IXAudio2EngineCallback, registered through
#      the CoCreateInstance lane (not xaudio2_9's flat XAudio2Create lane),
#      really is CALLED -- OnProcessingPassStart and OnProcessingPassEnd arrive
#      on XAudio2's mixer thread, with the `this` the guest registered and the
#      guest's own tag word readable through it, balanced, in that order, and
#      with no OnCriticalError.  Built from the same source as a native ppc64 PE
#      and as an x86-64 guest PE, and the two must print the same bytes: the
#      native leg is what says the API delivers these callbacks at all.
#   H  MECHANISM: the port's own +winecom trace of the guest run must show the
#      system-COM guest vtables materialised with the exact counts the roster
#      states, the IXAudio2 and IMMDeviceEnumerator wrapped, a VOICE wrapped
#      (which only happens inside a hand-written slot), the individual methods
#      arriving in the dispatcher by name, and the unrostered IID NAMED in the
#      log rather than silently refused.  Identical output through a mechanism
#      that never attached would mean the guest was calling native vtables
#      directly.  (The trace run is where the named-IID line can be looked for
#      at all: the untraced legs run under WINEDEBUG=-all, which suppresses even
#      an ERR.)
#
# --sabotage runs the negative controls instead, and requires every one to go
# red.  A gate that cannot go red proves nothing:
#
#   1  WINEEMUNOCOMWRAP=1 hands the guest RAW host pointers -- the exact defect
#      libs/winecom exists to fix -- and the guest leg must not PASS.
#   2  each SC_AUDIO_BREAK=1..3 build of the NATIVE leg must FAIL, so the value
#      checks are shown to be checks.
#   3  a roster whose IXAudio2 is given the 2.9 slot list must FAIL leg A, so
#      the version the whole gate turns on is shown to be checked.
#   4  a marshal header with an xmask smuggled onto one of the 19 hazard rows
#      must FAIL leg H2, so "they fail closed" is shown to be measured.
#   5  each SC_REV_BREAK=1..2 build of the NATIVE leg of the reverse probe must
#      FAIL: =1 claims the registration without making it -- the engine still
#      runs, so this asks whether the delivered callbacks came from OUR
#      registration or would have happened anyway; =2 expects the wrong `this`,
#      so the delivered-value check must notice.
#
# WHY EVERY WINE RUN DISABLES winedbg, verbatim from check-audio-smoke.sh
# because the hazard is identical: the bringup prefix has AeDebug configured
# with "winedbg --auto", so a run that ends in an unhandled fault -- which is
# what a defect here looks like from outside -- starts a debugger that attaches
# and never lets go, turning every red state of this gate into a hang.
#
# Exit 0 = pass, 1 = a check failed, 2 = could not run (a skip is NOT a pass).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$SRC}                    # in-tree build by default
OUT=${OUT:-/tmp/syscom-audio-smoke}
SABOTAGE=0
[ "${1:-}" = "--sabotage" ] && SABOTAGE=1

say()  { echo "check-syscom-audio: $*"; }
bad()  { echo "check-syscom-audio: FAIL $*" >&2; fail=1; }
skip() { echo "check-syscom-audio: $*" >&2; cleanup; exit 2; }

SINK_MODULE=
SINK_NAME=wine_syscom_audio_gate_$$
DEFAULT_SINK_SAVED=

# Never trust the captured module id alone: look it up and refuse to unload
# unless its own arguments name OUR sink.  This is what keeps a gate from ever
# unloading a module -- null sink or otherwise -- that belongs to the person
# using the machine (a real regression: a gate once tore down the user's own
# Sunshine streaming sink this way).
unload_own_sink() {
    [ -n "$SINK_MODULE" ] || return 0
    args=$(pactl list short modules 2>/dev/null \
        | awk -F'\t' -v id="$SINK_MODULE" '$1==id{print $3}')
    case "$args" in
        *"sink_name=$SINK_NAME"*) pactl unload-module "$SINK_MODULE" 2>/dev/null ;;
        "") ;;  # already gone -- nothing to unload
        *) echo "check-syscom-audio: refusing to unload module $SINK_MODULE: its \
arguments do not name sink $SINK_NAME (got: $args)" >&2 ;;
    esac
    SINK_MODULE=
}

# The default sink is never meant to change here, but restore it defensively
# in case some other layer (pipewire-pulse's own default-sink policy, e.g.)
# moved it out from under this gate when the null sink appeared.
restore_default_sink() {
    [ -n "$DEFAULT_SINK_SAVED" ] || return 0
    cur=$(pactl get-default-sink 2>/dev/null)
    [ "$cur" = "$DEFAULT_SINK_SAVED" ] && return 0
    pactl set-default-sink "$DEFAULT_SINK_SAVED" 2>/dev/null
}

cleanup() {
    unload_own_sink
    restore_default_sink
}
trap 'cleanup' EXIT INT TERM
DEFAULT_SINK_SAVED=$(pactl get-default-sink 2>/dev/null)

[ -x "$BUILD/wine" ] || skip "no wine loader at $BUILD/wine"
[ -n "${WINEPREFIX:-}" ] || skip "set WINEPREFIX to a prefix wineboot has run in"
[ -n "${WINEFEXBRIDGE:-}" ] || skip "set WINEFEXBRIDGE to the emulator bridge"
command -v clang >/dev/null || skip "need clang for the guest build"
command -v llvm-dlltool >/dev/null || skip "need llvm-dlltool for the guest build"
command -v python3 >/dev/null || skip "need python3 for the roster checks"
command -v pactl >/dev/null || skip "need pactl: this gate opens a null sink of \
its own and will not open one it did not create"

mkdir -p "$OUT" || skip "cannot create $OUT"
fail=0
TIMEOUT=${TIMEOUT:-180}
INCL="-I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt"
GEN="$HERE/gen_syscom_audio.py"
ROSTER="$HERE/interfaces_syscom.json"
MARSHAL="$SRC/dlls/combase/syscom_marshal.h"

# ---- A: the roster and the marshal table are current -----------------------
roster_leg() {
    if python3 "$GEN" --build "$BUILD" --roster --check "$ROSTER" \
            > "$OUT/roster.out" 2>&1; then
        say "roster: $(grep -m1 'audio family' "$OUT/roster.out")"
    else
        sed 's/^/  roster| /' "$OUT/roster.out" >&2
        bad "ppc64le/syscom/interfaces_syscom.json has drifted from Wine's headers"
    fi
    if python3 "$GEN" --build "$BUILD" --marshal --check "$MARSHAL" \
            > "$OUT/marshal.out" 2>&1; then
        say "marshal: $(grep -m1 'audio family' "$OUT/marshal.out")"
    else
        sed 's/^/  marshal| /' "$OUT/marshal.out" >&2
        bad "dlls/combase/syscom_marshal.h has drifted from the roster"
    fi
    if python3 "$GEN" --selfcheck "$MARSHAL" > "$OUT/selfcheck.out" 2>&1; then
        say "reuse: $(cat "$OUT/selfcheck.out")"
    else
        sed 's/^/  selfcheck| /' "$OUT/selfcheck.out" >&2
        bad "the marshal header does not survive a parse/re-emit round trip, so \
'the 58 reused blocks are verbatim' is not a checked statement"
    fi
}

# Interface and slot counts a guest thunk module publishes, read exactly the way
# libs/winecom's com_check_module() reads them at attach.
com_counts() {   # com_counts <guest dll> -> "<ifaces> <slots>"
    python3 - "$1" <<'EOF'
import struct, sys
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
n = struct.unpack_from('<I', d, e + 24)[0]
fn_rva, name_rva, ord_rva = struct.unpack_from('<III', d, e + 28)
info_rva = None
for i in range(n):
    r = struct.unpack_from('<I', d, off(name_rva) + 4 * i)[0]
    o = off(r)
    if d[o:d.index(b'\0', o)] == b'__wine_com_thunk_info':
        idx = struct.unpack_from('<H', d, off(ord_rva) + 2 * i)[0]
        info_rva = struct.unpack_from('<I', d, off(fn_rva) + 4 * idx)[0]
if info_rva is None:
    print("0 0"); raise SystemExit
io = off(info_rva)
ver, ic, stride, trap, ifr = struct.unpack_from('<IIIII', d, io)
slots = 0
for i in range(ic):
    slots += struct.unpack_from('<I', d, off(ifr) + i * 24 + 16)[0]
print("%d %d" % (ic, slots))
EOF
}

thunk_leg() {
    wi=$(python3 -c "import json;d=json.load(open('$ROSTER'));print(len(d['interfaces']))")
    ws=$(python3 -c "import json;d=json.load(open('$ROSTER'));print(sum(len(i['slots']) for i in d['interfaces'].values()))")
    for m in combase ole32 oleaut32; do
        dll="$BUILD/dlls/$m/x86_64-windows/$m.dll"
        [ -f "$dll" ] || { bad "no guest thunk at $dll; build it first"; continue; }
        set -- $(com_counts "$dll") 0 0
        if [ "$1" = "$wi" ] && [ "$2" = "$ws" ]; then
            say "thunk($m): $1 interface(s), $2 vtable slot(s)"
        else
            bad "the guest $m.dll publishes $1 interfaces / $2 slots, \
the roster says $wi / $ws -- a roster change does not rebuild the thunks (see \
leg B in this file's banner); touch dlls/$m/$m.thunks and rebuild"
        fi
    done
}

# ---- builds ----------------------------------------------------------------
# The native leg is built exactly the way check-com-smoke.sh builds its native
# lane: an ordinary consumer of the public headers -- not Wine source -- linked
# against this tree's own ppc64-windows import libraries and turned into a
# builtin PE.  It runs under the SAME wine as the guest leg.
native_build() {   # native_build <break> <output>
    native_build_src "$HERE/syscom_audio_smoke.c" "-DSC_AUDIO_BREAK=$1" "$2"
}

native_build_src() {   # native_build_src <source> <defines> <output>
    nsrc=$1; ndef=$2; nout=$3
    ${CC:-gcc} -c -o "$OUT/native.o" "$nsrc" $INCL \
        $ndef \
        -D_UCRT -D_WIN32 -Wall -pipe -mlongcall -mno-pltseq -fcf-protection=none \
        -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing -gdwarf-4 \
        -fPIC -fasynchronous-unwind-tables -mlong-double-64 -fno-builtin \
        -fshort-wchar -Wno-format -g -O1 2>"$OUT/native.build.err" || return 1
    "$BUILD/tools/winegcc/winegcc" -o "$nout" --wine-objdir "$BUILD" \
        --cc-cmd="${CC:-gcc}" -mno-cygwin -fPIC -fasynchronous-unwind-tables \
        -Wl,--wine-builtin -mconsole "$OUT/native.o" \
        "$BUILD/libs/winecrt0/ppc64-windows/libwinecrt0.a" \
        "$BUILD/dlls/ole32/ppc64-windows/libole32.a" \
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

# The guest leg: the same clang x86_64-windows-gnu machinery tools/spec2thunk
# drives its signature oracle with, and the same Wine headers, so any
# disagreement between the two legs is the boundary and not the declarations.
# The imports are described by hand, naming only what the probe calls: the guest
# binds to the same builtins a real application would, and there is no CRT.
guest_build() {
    guest_build_src "$HERE/syscom_audio_smoke.c" \
        "-DSC_AUDIO_NO_CRT -DSC_AUDIO_BREAK=0" sc_audio_entry "$OUT/guest.exe"
}

guest_build_src() {   # guest_build_src <source> <defines> <entry> <output>
    gsrc=$1; gdef=$2; gentry=$3; gout=$4
    cat > "$OUT/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
CoInitializeEx
CoUninitialize
CoCreateInstance
EOF
    cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
Sleep
EOF
    for m in ole32 kernel32; do
        llvm-dlltool -m i386:x86-64 -d "$OUT/$m.def" -l "$OUT/lib$m.a" \
            2>"$OUT/guest.build.err" || return 1
    done
    clang -target x86_64-windows-gnu -nostdlibinc $INCL \
        $gdef -D_UCRT -Wall -O1 -fno-builtin -g \
        -c -o "$OUT/guest.o" "$gsrc" \
        2>>"$OUT/guest.build.err" || return 1
    clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib \
        -Wl,--entry="$gentry" -Wl,--subsystem,console \
        -o "$gout" "$OUT/guest.o" \
        "$OUT/libole32.a" "$OUT/libkernel32.a" 2>>"$OUT/guest.build.err" || return 1
    return 0
}

# ---- the leg I probe -------------------------------------------------------
# Written out here rather than kept beside syscom_audio_smoke.c on purpose: it
# is the gate's own instrument, it exists only to be built twice and run twice,
# and keeping it in the checker means the claim and the thing that makes the
# claim cannot drift apart.
#
# WHAT IT MEASURES.  A guest program CoCreateInstances the 2.7 engine, hands it
# an IXAudio2EngineCallback THE GUEST IMPLEMENTS, starts the engine, and waits.
# XAudio2 then calls that object from its own mixer thread -- native ppc64 code
# entering x86-64 code through libs/winecom/reverse.c and the emulator.  What is
# checked is not "no error": it is that the callbacks ARRIVED, that each arrived
# with the `this` the guest registered, that the guest's own tag word is
# readable through that `this` (so the pointer is the guest object and not a
# wrapper that merely looks like one), that starts and ends are balanced and
# that a start came first, and that OnCriticalError never fired.
rev_probe_src() {
    cat > "$OUT/syscom_reverse_cb.c" <<'REV_EOF'
/* GENERATED BY ppc64le/syscom/check-syscom-audio.sh -- leg I.
 *
 * The REVERSE direction of the system-COM surface, through the CoCreateInstance
 * lane.  One source, built as a native ppc64 PE and as an x86-64 guest PE; the
 * two must print the same bytes.
 *
 * SC_REV_BREAK (falsification; the gate builds each variant of the NATIVE leg
 * and requires it to FAIL):
 *   =1  never start the engine, so no callback can arrive -- the delivery
 *       check must notice that nothing was delivered.
 *   =2  expect the wrong `this`, so the delivered-value check must notice. */

#define COBJMACROS
#include <windows.h>
#include <objbase.h>

#ifndef SC_REV_BREAK
#define SC_REV_BREAK 0
#endif

/* coclass XAudio2 at XAUDIO2_VER == 7, and the IID every version up to 2.7
 * shares -- spelled out because the guest build has no import libraries. */
static const GUID rv_CLSID_XAudio2 =
    { 0x5a508685, 0xa254, 0x4fba, { 0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf } };
static const GUID rv_IID_IXAudio2 =
    { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb } };

typedef struct RV_IXAudio2 RV_IXAudio2;
typedef struct RV_Voice RV_Voice;

/* A voice: 19 slots, of which only DestroyVoice (18) is called here.  Slot 0 is
 * GetVoiceDetails and not QueryInterface -- IXAudio2Voice is [local]. */
typedef struct
{
    void *slot[18];
    void (STDMETHODCALLTYPE *DestroyVoice)( RV_Voice * );
} RV_VoiceVtbl;
struct RV_Voice { const RV_VoiceVtbl *lpVtbl; };

/* THE 2.7 SHAPE: three slots at the head (GetDeviceCount/GetDeviceDetails/
 * Initialize) that 2.8 and later do not have, and a CreateMasteringVoice that
 * takes a device INDEX. */
typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( RV_IXAudio2 *, REFIID, void ** );
    ULONG   (STDMETHODCALLTYPE *AddRef)( RV_IXAudio2 * );
    ULONG   (STDMETHODCALLTYPE *Release)( RV_IXAudio2 * );
    HRESULT (STDMETHODCALLTYPE *GetDeviceCount)( RV_IXAudio2 *, UINT32 * );
    HRESULT (STDMETHODCALLTYPE *GetDeviceDetails)( RV_IXAudio2 *, UINT32, void * );
    HRESULT (STDMETHODCALLTYPE *Initialize)( RV_IXAudio2 *, UINT32, UINT32 );
    HRESULT (STDMETHODCALLTYPE *RegisterForCallbacks)( RV_IXAudio2 *, void * );
    void    (STDMETHODCALLTYPE *UnregisterForCallbacks)( RV_IXAudio2 *, void * );
    HRESULT (STDMETHODCALLTYPE *CreateSourceVoice)( RV_IXAudio2 *, void **,
                                                    const WAVEFORMATEX *, UINT32,
                                                    float, void *, const void *,
                                                    const void * );
    HRESULT (STDMETHODCALLTYPE *CreateSubmixVoice)( RV_IXAudio2 *, void **, UINT32,
                                                    UINT32, UINT32, UINT32,
                                                    const void *, const void * );
    HRESULT (STDMETHODCALLTYPE *CreateMasteringVoice)( RV_IXAudio2 *, RV_Voice **,
                                                       UINT32, UINT32, UINT32,
                                                       UINT32, const void * );
    HRESULT (STDMETHODCALLTYPE *StartEngine)( RV_IXAudio2 * );
    void    (STDMETHODCALLTYPE *StopEngine)( RV_IXAudio2 * );
    HRESULT (STDMETHODCALLTYPE *CommitChanges)( RV_IXAudio2 *, UINT32 );
    void    (STDMETHODCALLTYPE *GetPerformanceData)( RV_IXAudio2 *, void * );
    void    (STDMETHODCALLTYPE *SetDebugConfiguration)( RV_IXAudio2 *, const void *,
                                                        void * );
} RV_IXAudio2Vtbl;
struct RV_IXAudio2 { const RV_IXAudio2Vtbl *lpVtbl; };

/* ------------------------ the object THIS PROGRAM implements and hands over */

typedef struct RV_Engine RV_Engine;
typedef struct
{
    void (STDMETHODCALLTYPE *OnProcessingPassStart)( RV_Engine * );
    void (STDMETHODCALLTYPE *OnProcessingPassEnd)( RV_Engine * );
    void (STDMETHODCALLTYPE *OnCriticalError)( RV_Engine *, HRESULT );
} RV_EngineVtbl;

/* The tag is the point of the struct having a second member at all: reading it
 * back through the `this` the callback was entered with proves the pointer
 * really addresses THIS object's storage, which a wrapper that merely compared
 * equal would not. */
#define RV_TAG 0x5A17C0DEu
struct RV_Engine { const RV_EngineVtbl *lpVtbl; ULONG tag; };

/* Written from XAudio2's mixer thread and read from the main one after the
 * engine has been stopped, which is the only ordering this needs. */
static volatile LONG rv_starts, rv_ends, rv_errors, rv_wrong_this, rv_first;

static RV_Engine rv_cb;

/* SC_REV_BREAK=2 expects the wrong object, so the value check has something to
 * catch.  It is spelled as a function rather than a constant so that the two
 * builds differ in one place only. */
static RV_Engine *rv_expected( void )
{
#if SC_REV_BREAK == 2
    return (RV_Engine *)((char *)&rv_cb + 16);
#else
    return &rv_cb;
#endif
}

static void rv_seen( RV_Engine *self, LONG kind )
{
    if (self != rv_expected() || self->tag != RV_TAG) rv_wrong_this++;
    if (!rv_first) rv_first = kind;
}

static void STDMETHODCALLTYPE rv_on_start( RV_Engine *self )
{
    rv_seen( self, 1 );
    rv_starts++;
}

static void STDMETHODCALLTYPE rv_on_end( RV_Engine *self )
{
    rv_seen( self, 2 );
    rv_ends++;
}

static void STDMETHODCALLTYPE rv_on_error( RV_Engine *self, HRESULT hr )
{
    (void)hr;
    rv_seen( self, 3 );
    rv_errors++;
}

static const RV_EngineVtbl rv_cb_vtbl = { rv_on_start, rv_on_end, rv_on_error };
static RV_Engine rv_cb = { &rv_cb_vtbl, RV_TAG };

/* ----------------------------------------------------------------- output */

static void rv_write( HANDLE h, const char *s )
{
    DWORD n = 0, written;

    while (s[n]) n++;
    WriteFile( h, s, n, &written, NULL );
}

static void out( const char *s ) { rv_write( GetStdHandle( STD_OUTPUT_HANDLE ), s ); }
static void err( const char *s ) { rv_write( GetStdHandle( STD_ERROR_HANDLE ), s ); }

static void fmt_hex( char *buf, ULONGLONG v, int digits )
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xf];
    buf[digits] = 0;
}

static void out_hex8( ULONG v ) { char b[9]; fmt_hex( b, v, 8 ); out( b ); }

static void err_dec( const char *label, LONG v )
{
    char b[24];
    int i = 23;
    ULONG u = (ULONG)(v < 0 ? -v : v);

    err( label );
    b[i] = 0;
    do { b[--i] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    err( b + i );
    err( "\n" );
}

static int rv_failures;

static void flag( const char *name, int ok )
{
    out( " " );
    out( name );
    out( ok ? "=1" : "=0" );
    if (!ok) rv_failures++;
}

/* -------------------------------------------------------------------- run */

static int rv_run( void )
{
    RV_IXAudio2 *xa2 = NULL;
    RV_Voice *master = NULL;
    HRESULT hr, reg_hr = E_FAIL;
    LONG starts, ends, errors, wrong, first;

    hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    if (FAILED(hr)) { out( "syscom_reverse_cb: FAIL CoInitializeEx\n" ); return 1; }

    hr = CoCreateInstance( &rv_CLSID_XAudio2, NULL, CLSCTX_INPROC_SERVER,
                           &rv_IID_IXAudio2, (void **)&xa2 );
    if (FAILED(hr) || !xa2)
    {
        out( "syscom_reverse_cb: FAIL CoCreateInstance(CLSID_XAudio2)=0x" );
        out_hex8( (ULONG)hr );
        out( "\n" );
        CoUninitialize();
        return 1;
    }

    hr = xa2->lpVtbl->Initialize( xa2, 0, 0xffffffffu /* ANY_PROCESSOR */ );
    if (SUCCEEDED(hr))
        hr = xa2->lpVtbl->CreateMasteringVoice( xa2, &master, 0, 0, 0,
                                                0 /* device index */, NULL );
    if (FAILED(hr) || !master)
    {
        out( "syscom_reverse_cb: FAIL engine setup=0x" );
        out_hex8( (ULONG)hr );
        out( "\n" );
        goto done;
    }

    /* THE CALL THIS WHOLE GATE LEG EXISTS FOR.  &rv_cb is a COM object this
     * program implements; on the guest leg it is x86-64 code, and serving this
     * means libs/winecom builds a native vtable around it for XAudio2's mixer
     * thread to call. */
#if SC_REV_BREAK == 1
    /* Claim the registration without making it.  The engine still runs and the
     * mastering voice still mixes, so this isolates ONE question: do the counts
     * below come from OUR registration, or from something that would have
     * happened anyway?  Every flag but the two delivery ones stays green. */
    reg_hr = S_OK;
#else
    reg_hr = xa2->lpVtbl->RegisterForCallbacks( xa2, &rv_cb );
#endif

    xa2->lpVtbl->StartEngine( xa2 );
    /* Long enough for many mixer quanta at any sane quantum size, and bounded
     * so a surface that never calls back is a red result rather than a hang. */
    Sleep( 400 );
    xa2->lpVtbl->StopEngine( xa2 );

    /* The unregister has to reach the SAME registration, which on the guest leg
     * means the same reverse proxy -- reverse proxies are interned by (guest
     * pointer, interface) exactly so that this works. */
#if SC_REV_BREAK != 1
    if (SUCCEEDED(reg_hr)) xa2->lpVtbl->UnregisterForCallbacks( xa2, &rv_cb );
#endif

done:
    starts = rv_starts; ends = rv_ends; errors = rv_errors;
    wrong  = rv_wrong_this; first = rv_first;

    if (master) master->lpVtbl->DestroyVoice( master );
    if (xa2) xa2->lpVtbl->Release( xa2 );
    CoUninitialize();

    /* The counts themselves are a property of the machine and the quantum, so
     * they go to stderr and are never diffed.  What goes to stdout is what must
     * be identical on both legs. */
    err_dec( "note: OnProcessingPassStart calls = ", starts );
    err_dec( "note: OnProcessingPassEnd calls   = ", ends );
    err_dec( "note: OnCriticalError calls       = ", errors );
    err_dec( "note: calls with the wrong `this` = ", wrong );

    out( "syscom_reverse_cb: register=0x" );
    out_hex8( (ULONG)reg_hr );
    flag( "registered", SUCCEEDED(reg_hr) );
    flag( "starts_arrived", starts > 0 );
    flag( "ends_arrived", ends > 0 );
    flag( "balanced", starts - ends <= 1 && ends - starts <= 1 );
    flag( "this_ok", wrong == 0 );
    flag( "start_came_first", first == 1 );
    flag( "no_critical_error", errors == 0 );
    out( "\n" );
    out( rv_failures ? "syscom_reverse_cb: FAIL\n" : "syscom_reverse_cb: PASS\n" );
    return rv_failures ? 1 : 0;
}

#ifdef SC_REV_NO_CRT
/* The guest build has no C runtime: this IS the image entry point. */
void WINAPI rv_entry( void )
{
    ExitProcess( (UINT)rv_run() );
}
#else
int main( void )
{
    return rv_run();
}
#endif
REV_EOF
}

# ---- leg H2: the reused slots that must stay closed ------------------------
# THE 19.  Named here in full rather than computed, because a list the checker
# derived with the same rule the generator used would agree with it by
# construction and prove nothing.  These are every CA_IFACE_IN-bearing slot of
# the four reused interfaces a guest application really implements and hands to
# native COM.  Each is checked twice: in the shipped marshal header (its row
# must stop at aux2 -- no xmask, so libs/winecom reads no interface type it was
# never given, and refuses by name) and in gen_syscom_audio.py's own generated
# withheld list.
HAZARD_SLOTS="IMoniker:5 IMoniker:6 IMoniker:8 IMoniker:9 IMoniker:10 \
IMoniker:11 IMoniker:13 IMoniker:15 IMoniker:16 IMoniker:18 IMoniker:19 \
IMoniker:20 IRunningObjectTable:3 IRunningObjectTable:5 IRunningObjectTable:6 \
IRunningObjectTable:8 IDirectMusicTool:3 IDirectMusicTrack:3 IDirectMusicTrack:6"

hazard_check() {   # hazard_check <marshal header> -> prints, exit 0 = closed
    python3 - "$1" "$HAZARD_SLOTS" <<'EOF'
import re, sys

text = open(sys.argv[1]).read()
want = [w.split(":") for w in sys.argv[2].split()]
bad = []
n_in = 0
for iface, slot in want:
    slot = int(slot)
    m = re.search(r'static const struct winecom_slot slots_%s\[\d+\] =\n\{\n(.*?)\n\};'
                  % iface, text, re.S)
    if not m:
        bad.append("%s has no slot table" % iface)
        continue
    # Rows in slot order.  A row STARTS at a line beginning `    { "` and runs
    # to the next one, because a refusal row spans three lines and an IUnknown
    # row ends with a trailing `/* runtime */` comment -- neither of which a
    # single-line pattern would collect.
    body = m.group(1).split("\n")
    rows, cur = [], None
    for line in body:
        if line.startswith('    { "'):
            if cur is not None: rows.append(cur)
            cur = line
        elif line.strip().startswith("/*"):
            # the reclassification pass's marker comments sit BETWEEN rows;
            # gluing one onto the previous row shifted every later slot
            continue
        elif cur is not None:
            cur += " " + line.strip()
    if cur is not None: rows.append(cur)
    if slot >= len(rows):
        bad.append("%s slot %d is past its table" % (iface, slot))
        continue
    row = " ".join(rows[slot].split())
    # A reused row is name, refuse, cls, xaux, argc, flags, aux, aux2 -- eight
    # fields and nothing after them.  fpmask/fpwide/xmask would be a ninth,
    # tenth and eleventh, and it is the eleventh that would open the door.
    fields = row.rstrip("},").split(",")
    if len(fields) > 8:
        # A full-form row in a reused block is the reclassification pass's
        # (2026-09-01).  The hazard is not the FIELD but the BIT: the pass
        # applies the same REVERSE_SINKS licence as every owned row, so the
        # emitted xmask -- the last field -- must still be zero here.
        xmask = int(fields[-1].strip(), 16)
        if xmask:
            bad.append("%s slot %d carries xmask %#x -- a licence bit reached "
                       "a hazard row (%s)" % (iface, slot, xmask, row))
            continue
    n_in += 1
print("%d of %d hazard rows stop at aux2 (no xmask, so every interface "
      "IN-parameter in them fails closed)" % (n_in, len(want)))
for b in bad:
    print("hazard: " + b, file=sys.stderr)
sys.exit(1 if bad else 0)
EOF
}

hazard_leg() {
    if hazard_check "$MARSHAL" > "$OUT/hazard.out" 2>"$OUT/hazard.err"; then
        say "hazard: $(cat "$OUT/hazard.out")"
    else
        sed 's/^/  hazard| /' "$OUT/hazard.err" >&2
        bad "a reused hazard row carries an xmask; a guest-implemented object \
at that position would be given a native vtable this port cannot serve"
    fi
    # ...and the generator must NAME each of them as withheld, so the header
    # check above and the generator's own judgement are two sources and not one.
    python3 "$GEN" --build "$BUILD" --marshal --check "$MARSHAL" --report \
        > "$OUT/withheld.out" 2>&1 || true
    miss=""
    for hs in $HAZARD_SLOTS; do
        hi=${hs%%:*}; hn=${hs##*:}
        grep -qE "^  $hi +slot +$hn +param" "$OUT/withheld.out" \
            || miss="$miss $hs"
    done
    if [ -n "$miss" ]; then
        bad "gen_syscom_audio.py --report does not name these slots as \
reverse-proxy WITHHELD:$miss"
    else
        say "hazard: gen_syscom_audio.py names all $(echo $HAZARD_SLOTS | wc -w) \
of them in its generated withheld list ($(grep -cE '^  I[A-Za-z0-9]+ +slot +[0-9]+ +param' \
"$OUT/withheld.out") withheld interface IN-parameters in total)"
    fi
}

# ---- C: a null sink of this gate's own -------------------------------------
SINK_MODULE=$(pactl load-module module-null-sink sink_name="$SINK_NAME" \
    sink_properties=device.description="$SINK_NAME" 2>"$OUT/sink.err") \
    || { sed 's/^/  pactl| /' "$OUT/sink.err" >&2
         skip "cannot create a null sink; this gate will not open one it did \
not create" ; }

# Bounded, because a run that never returns is a result too: the sabotage
# control hands the guest a native code pointer and it can spin instead of
# faulting, and an unbounded gate would hang there rather than report red.
run() {   # run <exe> <stdout> <stderr> [WINEDEBUG] [NOCOMWRAP]
    exe=$1; o=$2; e=$3
    timeout -k 5 "$TIMEOUT" env WINEDEBUG="${4:--all}" \
        WINEEMUNOCOMWRAP="${5:-0}" WINEDLLOVERRIDES="winedbg.exe=d" \
        PULSE_SINK="$SINK_NAME" "$BUILD/wine" "$exe" > "$o" 2> "$e"
}

if [ "$SABOTAGE" = 1 ]; then
    # ---- 1: raw interface pointers ----------------------------------------
    guest_build || { sed 's/^/  guest| /' "$OUT/guest.build.err" >&2
                     skip "the guest build failed"; }
    run "$OUT/guest.exe" "$OUT/sab_nowrap.out" "$OUT/sab_nowrap.err" +winecom 1
    if grep -q "syscom_audio_smoke: PASS" "$OUT/sab_nowrap.out"; then
        bad "WINEEMUNOCOMWRAP=1 still PASSED -- the gate cannot go red"
    else
        say "sabotage(nowrap): raw interface pointers failed the guest run at \
'$(tail -1 "$OUT/sab_nowrap.out" | cut -c1-60)', as they must"
    fi

    # ---- 2: each value check must be able to fail -------------------------
    for brk in 1 2 3; do
        if ! native_build "$brk" "$OUT/native_brk$brk.exe"; then
            sed 's/^/  build| /' "$OUT/native.build.err" >&2
            bad "SC_AUDIO_BREAK=$brk did not build"
            continue
        fi
        run "$OUT/native_brk$brk.exe" "$OUT/sab_brk$brk.out" "$OUT/sab_brk$brk.err"
        if grep -q "syscom_audio_smoke: PASS" "$OUT/sab_brk$brk.out"; then
            bad "SC_AUDIO_BREAK=$brk still PASSED -- that value check is a print"
        else
            say "sabotage(break $brk): $(grep -m1 'syscom_audio_smoke: FAIL' \
                "$OUT/sab_brk$brk.out" | cut -c1-72)"
        fi
    done

    # ---- 3: the version the whole gate turns on ---------------------------
    # IXAudio2 given the 2.9 slot list -- the roster's headline hazard, and the
    # one nothing else in the tree would catch.
    python3 - "$ROSTER" "$OUT/sab_ver.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
s = d["interfaces"]["IXAudio2"]["slots"]
# drop GetDeviceCount/GetDeviceDetails/Initialize, which is exactly what
# XAUDIO2_VER >= 8 does, and renumber
del s[3:6]
for n, x in enumerate(s):
    x["slot"] = n
# written with the generator's own serialisation, so the ONLY difference from a
# regeneration is the slot list and the control cannot pass by formatting
open(sys.argv[2], "w").write(json.dumps(d, indent=2, sort_keys=False) + "\n")
EOF
    if python3 "$GEN" --build "$BUILD" --roster --check "$OUT/sab_ver.json" \
            > "$OUT/sab_ver.out" 2>&1; then
        bad "a 2.9-shaped IXAudio2 PASSED the roster check -- the version is \
not checked anywhere"
    else
        say "sabotage(version): a 2.9-shaped IXAudio2 failed the roster check, \
as it must"
    fi

    # ---- 4: a hazard row given an xmask must fail leg H2 ------------------
    # The bit is smuggled onto IMoniker::IsRunning, whose three IN-parameters
    # are a guest's own bind context and two monikers -- exactly the shape the
    # licence exists to refuse.
    python3 - "$MARSHAL" "$OUT/sab_xmask.h" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
row = '    { "IMoniker::IsRunning", NULL, cls_IMoniker_15, NULL, 4, 0, 0, 0 },'
if row not in text:
    sys.exit("sabotage: IMoniker::IsRunning is not the row this control edits")
open(sys.argv[2], "w").write(text.replace(
    row,
    '    { "IMoniker::IsRunning", NULL, cls_IMoniker_15, xaux_IMoniker_15, 4, '
    '0, 0, 0, NULL, 0x00, 0x00, 0x07 },'))
EOF
    if [ ! -f "$OUT/sab_xmask.h" ]; then
        bad "could not build the xmask sabotage header"
    elif hazard_check "$OUT/sab_xmask.h" >"$OUT/sab_xmask.out" 2>&1; then
        bad "an xmask on a hazard row PASSED the hazard check -- leg H2 cannot \
go red, so 'the 19 fail closed' is not a measurement"
    else
        say "sabotage(xmask): an xmask smuggled onto IMoniker::IsRunning failed \
the hazard check, as it must"
    fi

    # ---- 5: the reverse probe's checks must be able to fail ----------------
    rev_probe_src
    for brk in 1 2; do
        if ! native_build_src "$OUT/syscom_reverse_cb.c" "-DSC_REV_BREAK=$brk" \
                "$OUT/rev_brk$brk.exe"; then
            sed 's/^/  build| /' "$OUT/native.build.err" >&2
            bad "SC_REV_BREAK=$brk did not build"
            continue
        fi
        run "$OUT/rev_brk$brk.exe" "$OUT/sab_rev$brk.out" "$OUT/sab_rev$brk.err"
        if grep -q "syscom_reverse_cb: PASS" "$OUT/sab_rev$brk.out"; then
            bad "SC_REV_BREAK=$brk still PASSED -- that callback check is a print"
        else
            say "sabotage(rev $brk): $(head -1 "$OUT/sab_rev$brk.out" | cut -c1-110)"
        fi
    done

    [ $fail -eq 0 ] && say "SABOTAGE PASS (all controls red)"
    cleanup
    exit $fail
fi

say "sink: $SINK_NAME (module $SINK_MODULE), unloaded on the way out; the \
default sink is untouched and no existing sink is opened"

roster_leg
hazard_leg
thunk_leg

# ---- D: native -------------------------------------------------------------
native_build 0 "$OUT/native.exe" || { sed 's/^/  build| /' "$OUT/native.build.err" >&2
                                      skip "the native ppc64 build failed"; }
guest_build || { sed 's/^/  build| /' "$OUT/guest.build.err" >&2
                 skip "the x86-64 guest build failed"; }

run "$OUT/native.exe" "$OUT/native.out" "$OUT/native.err"
if grep -q "syscom_audio_smoke: PASS" "$OUT/native.out"; then
    say "native: $(tail -1 "$OUT/native.out")"
else
    sed 's/^/  native| /' "$OUT/native.out" >&2
    tail -20 "$OUT/native.err" >&2
    bad "the native ppc64 build did not pass"
fi

# ---- E: guest --------------------------------------------------------------
run "$OUT/guest.exe" "$OUT/guest.out" "$OUT/guest.err"
if grep -q "syscom_audio_smoke: PASS" "$OUT/guest.out"; then
    say "guest:  $(tail -1 "$OUT/guest.out")"
else
    sed 's/^/  guest| /' "$OUT/guest.out" >&2
    tail -20 "$OUT/guest.err" >&2
    bad "the x86-64 guest build did not pass"
fi

# ---- F: identity -----------------------------------------------------------
if cmp -s "$OUT/native.out" "$OUT/guest.out"; then
    say "identity: native and guest output is byte-identical ($(wc -l \
        < "$OUT/native.out") lines)"
else
    diff "$OUT/native.out" "$OUT/guest.out" >&2
    bad "native and guest output differ"
fi

# ---- G: what is now SERVED --------------------------------------------------
# IMMDevice::OpenPropertyStore USED TO BE HERE as the one still-refused note
# (E_NOTIMPL, "an interface this roster does not carry").  Cyberpunk 2077
# falsified the survivability of that refusal -- it calls OpenPropertyStore
# without checking the HRESULT, so the honest E_NOTIMPL left its
# IPropertyStore* uninitialised and the game called through stack garbage
# (run 30, 2026-08-19).  IPropertyStore is on the roster now and the property
# read path is a DIFFED STEP of this smoke: both legs walk GetCount / GetAt /
# GetValue and section F requires the answers byte-identical, which is a
# stronger statement than any grep here could make.  SetValue stays refused by
# name in the table (a guest-authored PROPVARIANT can carry VT_UNKNOWN).
#
# CreateSourceVoice-with-a-callback also used to be here and has moved down to
# the served list.  Its refusal was never a signature fact either: it was the
# roster gap that kept IXAudio2VoiceCallback -- the per-BUFFER sink a 2.7
# streaming loader waits on -- off the surface, and closing that gap is what
# lets the slot answer S_OK.  The sends/chain refusals beside it are untouched
# and are still signature facts, refused in both directions.
# THE THREE THAT ARE NOW SERVED.  All used to be in the list above.  The bar is
# not "the guest did not get E_NOTIMPL" but "the guest got the SAME answer the
# native leg got", which for the unregister is mmdevapi's own E_NOTFOUND
# (0x80070490) for a client that was never registered -- reached WITHOUT the
# pointer being translated, because mmdevapi's contract lets it be an address
# that is not an object (dlls/combase/syscom.c's notification registry).
for want in "IXAudio2::RegisterForCallbacks -> 0x00000000" \
            "IXAudio2::CreateSourceVoice with an IXAudio2VoiceCallback -> 0x00000000" \
            "IMMDeviceEnumerator::UnregisterEndpointNotificationCallback -> 0x80070490"; do
    for leg in guest native; do
        if ! grep -qF "note: $want" "$OUT/$leg.err"; then
            bad "the $leg leg did not answer '$want'; it answered \
'$(grep -F "$(echo "$want" | sed 's/ -> .*//')" "$OUT/$leg.err" | tail -1)'"
        fi
    done
done
# The OTHER kind of refusal, and the one that names the defect this whole
# roster addition fixes: an IID the roster does not carry, released by
# winecom_wrap_out_iface rather than handed over.  E_NOINTERFACE is 0x80004002.
if ! grep -qF "note: IMMDevice::Activate(IID_IAudioClient2, unrostered) -> 0x80004002" \
        "$OUT/guest.err"; then
    bad "the guest was not answered E_NOINTERFACE for an unrostered IID; it got \
'$(grep -F "IAudioClient2" "$OUT/guest.err" | tail -1)'"
fi
if ! grep -qF "note: IMMDevice::Activate(IID_IAudioClient2, unrostered) -> 0x00000000" \
        "$OUT/native.err"; then
    bad "the NATIVE leg did not get an IAudioClient2, so the guest being refused \
one proves nothing"
fi
[ $fail -eq 0 ] && say "refusals: the two signature-refusals answered E_NOTIMPL \
to the guest and succeeded natively, the two reverse-proxy registrations \
answered the guest exactly what they answered natively, and an unrostered IID \
was released with its GUID named"

# ---- I: the reverse direction, through the CoCreate lane -------------------
rev_probe_src
if ! native_build_src "$OUT/syscom_reverse_cb.c" "-DSC_REV_BREAK=0" \
        "$OUT/rev_native.exe"; then
    sed 's/^/  build| /' "$OUT/native.build.err" >&2
    bad "the native ppc64 build of the reverse probe failed"
elif ! guest_build_src "$OUT/syscom_reverse_cb.c" \
        "-DSC_REV_NO_CRT -DSC_REV_BREAK=0" rv_entry "$OUT/rev_guest.exe"; then
    sed 's/^/  build| /' "$OUT/guest.build.err" >&2
    bad "the x86-64 guest build of the reverse probe failed"
else
    run "$OUT/rev_native.exe" "$OUT/rev_native.out" "$OUT/rev_native.err"
    run "$OUT/rev_guest.exe"  "$OUT/rev_guest.out"  "$OUT/rev_guest.err"
    if ! grep -q "syscom_reverse_cb: PASS" "$OUT/rev_native.out"; then
        sed 's/^/  rev native| /' "$OUT/rev_native.out" >&2
        tail -5 "$OUT/rev_native.err" >&2
        bad "the NATIVE leg did not receive its engine callbacks, so the guest \
receiving them would prove nothing about the boundary"
    elif ! grep -q "syscom_reverse_cb: PASS" "$OUT/rev_guest.out"; then
        sed 's/^/  rev guest| /' "$OUT/rev_guest.out" >&2
        tail -20 "$OUT/rev_guest.err" >&2
        bad "a GUEST-implemented IXAudio2EngineCallback registered through \
CoCreateInstance did not receive OnProcessingPassStart/OnProcessingPassEnd \
with the values it registered"
    elif ! cmp -s "$OUT/rev_native.out" "$OUT/rev_guest.out"; then
        diff "$OUT/rev_native.out" "$OUT/rev_guest.out" >&2
        bad "the reverse probe's native and guest verdicts differ"
    else
        say "reverse: $(head -1 "$OUT/rev_guest.out")"
        say "reverse: guest callbacks delivered -- \
$(grep -h 'note:' "$OUT/rev_guest.err" | tr -s ' ' | tr '\n' ';' | sed 's/note: //g')"
    fi
fi

# ---- H: mechanism ----------------------------------------------------------
run "$OUT/guest.exe" "$OUT/guest.trace.out" "$OUT/guest.trace.err" +winecom
cmp -s "$OUT/native.out" "$OUT/guest.trace.out" || \
    bad "the traced guest run did not reproduce the untraced one"
WANT_IF=$(python3 -c "import json;d=json.load(open('$ROSTER'));print(len(d['interfaces']))")
WANT_SLOT=$(python3 -c "import json;d=json.load(open('$ROSTER'));print(sum(len(i['slots']) for i in d['interfaces'].values()))")
for want in "syscom: materialised $WANT_SLOT guest vtable slots across $WANT_IF interfaces" \
            "wrapped IXAudio2 host .* as proxy" \
            "wrapped IXAudio2MasteringVoice host .* as proxy" \
            "wrapped IXAudio2SourceVoice host .* as proxy" \
            "wrapped IMMDeviceEnumerator host .* as proxy" \
            "wrapped IMMDevice host .* as proxy" \
            "wrapped IAudioClient host .* as proxy" \
            "wrapped IAudioRenderClient host .* as proxy" \
            "an interface for unknown IID .726778cd-" \
            "syscom: reverse proxies armed" \
            "reverse-wrapped guest IXAudio2EngineCallback .* as native proxy" \
            "IXAudio2::Initialize " \
            "IXAudio2::GetDeviceCount " \
            "IMMDeviceEnumerator::GetDefaultAudioEndpoint " \
            "IAudioClient::GetService "; do
    if ! grep -qE "$want" "$OUT/guest.trace.err"; then
        bad "no '$want' in the +winecom trace of the guest run"
    fi
done
[ $fail -eq 0 ] && say "mechanism: system-COM guest vtables materialised \
($WANT_IF interfaces, $WANT_SLOT slots), engine/endpoint/client/voice wrapped, \
methods dispatched by name, and the guest's own engine callback given a NATIVE \
vtable by the reverse half"

[ $fail -eq 0 ] && say "PASS"
cleanup
exit $fail
