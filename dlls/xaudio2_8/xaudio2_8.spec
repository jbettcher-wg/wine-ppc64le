1 stdcall -ordinal XAudio2Create(ptr long long)
2 stdcall -ordinal CreateAudioReverb(ptr)
3 stdcall -ordinal CreateAudioVolumeMeter(ptr)
4 cdecl -ordinal CreateFX(ptr ptr ptr long)
5 cdecl -ordinal X3DAudioCalculate(ptr ptr ptr long ptr)
6 cdecl -ordinal X3DAudioInitialize(long float ptr)
7 stdcall -ordinal XAudio2CreateWithVersionInfo(ptr long long long)

# The x86-64 guest boundary (dlls/xaudio2_7/guestcom.c, shared with
# xaudio2_9 through PARENTSRC).  Private entry points, never imported by an
# application; see dlls/xaudio2_9/xaudio2_9.spec for what each one is.
@ stdcall -private __wine_com_dispatch(long long ptr)
@ stdcall -private __wine_com_refuse()
@ stdcall -private __wine_guest_XAudio2Create(ptr long long)
@ stdcall -private __wine_guest_XAudio2CreateWithVersionInfo(ptr long long long)
@ stdcall -private __wine_guest_CreateAudioReverb(ptr)
@ stdcall -private __wine_guest_CreateAudioVolumeMeter(ptr)
@ cdecl -private __wine_guest_CreateFX(ptr ptr ptr long)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall -private __wine_com_slot_name(long long ptr ptr)
