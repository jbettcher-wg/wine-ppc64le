1 stdcall DirectSoundCreate(ptr ptr ptr)
2 stdcall DirectSoundEnumerateA(ptr ptr)
3 stdcall DirectSoundEnumerateW(ptr ptr)
6 stdcall DirectSoundCaptureCreate(ptr ptr ptr)
7 stdcall DirectSoundCaptureEnumerateA(ptr ptr)
8 stdcall DirectSoundCaptureEnumerateW(ptr ptr)
9 stdcall GetDeviceID(ptr ptr)
10 stdcall DirectSoundFullDuplexCreate(ptr ptr ptr ptr long long ptr ptr ptr ptr)
11 stdcall DirectSoundCreate8(ptr ptr ptr)
12 stdcall DirectSoundCaptureCreate8(ptr ptr ptr)
@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()

# The x86-64 guest boundary (dlls/dsound/guestcom.c).  Private entry points,
# never imported by an application:
#   __wine_com_dispatch   the single entry ntdll's trap dispatcher calls when
#                         a guest calls a method on a DirectSound proxy
#   __wine_com_refuse     what a GUEST-REFUSE flat export resolves to
#   __wine_guest_<Name>   what the guest reaches instead of <Name>, via
#                         spec2thunk's GUEST-IMPL redirect; it wraps the
#                         interface pointers <Name> wrote.  <Name> itself is
#                         untouched, because a NATIVE ppc64 caller handed a
#                         proxy would execute the guest's x86-64 trap stubs.
@ stdcall -private __wine_com_dispatch(long long ptr)
@ stdcall -private __wine_com_refuse()
@ stdcall -private __wine_guest_DirectSoundCreate(ptr ptr ptr)
@ stdcall -private __wine_guest_DirectSoundCreate8(ptr ptr ptr)
@ stdcall -private __wine_guest_DirectSoundCaptureCreate(ptr ptr ptr)
@ stdcall -private __wine_guest_DirectSoundCaptureCreate8(ptr ptr ptr)
@ stdcall -private __wine_guest_DirectSoundFullDuplexCreate(ptr ptr ptr ptr long long ptr ptr ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall -private __wine_com_slot_name(long long ptr ptr)
