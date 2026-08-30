# d3d9.dll -- the native ppc64le D3D9 lane over DXVK's d3d9 (dlls/d3d9/main.c).
#
# Two names per interface-bearing entry point, and the split is the point --
# dlls/d3d11/d3d11.spec explains it at length and this is the same rule:
#
#   __wine_guest_<Name>   what the emulated x86-64 guest reaches, via the
#                         GUEST-IMPL rows in d3d9.thunks.  Hands back winecom
#                         proxies.
#   <Name>                what a NATIVE ppc64 PE would reach.  Refuses loudly:
#                         a proxy's vtable is the guest module's x86-64 trap
#                         stubs, so there is nothing correct to give it.
#
# __wine_com_dispatch is the single entry ntdll's trap dispatcher calls to
# serve a guest COM vtable slot.  Unlike the D3D11 lane there is nothing to
# forward it from: D3D9 is one DLL and one winecom instance.
@ stdcall D3DPERF_BeginEvent(long wstr)
@ stdcall D3DPERF_EndEvent()
@ stdcall D3DPERF_GetStatus()
@ stdcall D3DPERF_QueryRepeatFrame()
@ stdcall D3DPERF_SetMarker(long wstr)
@ stdcall D3DPERF_SetOptions(long)
@ stdcall D3DPERF_SetRegion(long wstr)
@ stub DebugSetLevel
@ stdcall DebugSetMute()
@ stdcall Direct3DCreate9(long)
@ stdcall Direct3DCreate9Ex(long ptr)
@ stdcall Direct3DCreate9On12(long ptr long)
@ stdcall Direct3DCreate9On12Ex(long ptr long ptr)
@ stdcall Direct3DShaderValidatorCreate9()
@ stub PSGPError
@ stub PSGPSampleTexture

@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_guest_Direct3DCreate9(long)
@ stdcall __wine_guest_Direct3DCreate9Ex(long ptr)
@ stdcall __wine_guest_Direct3DCreate9On12(long ptr long)
@ stdcall __wine_guest_Direct3DCreate9On12Ex(long ptr long ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr)

# The i386 twin of __wine_com_dispatch, appended after it for the same
# no-renumbering reason.  ntdll's 32-bit trap dispatcher calls THIS one; the
# CONTEXT it hands over is an I386_CONTEXT and the stdcall pop belongs to
# libs/winecom, not to the caller.  See dlls/d3d11/d3d11.spec, which grew the
# same pair when the D3D11 surface opened its 32-bit lane.
@ stdcall __wine_com_dispatch32(long long ptr)
