# d3d11.dll -- the native ppc64le D3D11 lane over DXVK (dlls/d3d11/main.c).
#
# Two names per interface-bearing entry point, and the split is the point:
#
#   __wine_guest_<Name>   what the emulated x86-64 guest reaches, via the
#                         GUEST-IMPL rows in d3d11.thunks.  Translates
#                         interface pointers and hands back winecom proxies.
#   <Name>                what a NATIVE ppc64 PE would reach.  Refuses loudly:
#                         a proxy's vtable is the guest module's x86-64 trap
#                         stubs, so there is nothing correct to give it.
#
# __wine_com_dispatch is the single entry ntdll's trap dispatcher calls to
# serve a guest COM vtable slot; dxgi.dll and d3d10core.dll forward theirs
# here, because this module owns the only winecom instance for the surface.
@ stdcall D3D11CoreCreateDevice(ptr ptr long ptr long ptr)
@ stub D3D11CoreCreateLayeredDevice
@ stub D3D11CoreGetLayeredDeviceSize
@ stdcall D3D11CoreRegisterLayers()
@ stdcall D3D11CreateDevice(ptr long ptr long ptr long long ptr ptr ptr)
@ stdcall D3D11CreateDeviceAndSwapChain(ptr long ptr long ptr long long ptr ptr ptr ptr ptr)
@ stdcall D3D11On12CreateDevice(ptr long ptr long ptr long long ptr ptr ptr)
@ stdcall D3DKMTCheckVidPnExclusiveOwnership(ptr) gdi32.D3DKMTCheckVidPnExclusiveOwnership
@ stdcall D3DKMTCloseAdapter(ptr) gdi32.D3DKMTCloseAdapter
@ stub D3DKMTCreateAllocation
@ stub D3DKMTCreateContext
@ stdcall D3DKMTCreateDevice(ptr) gdi32.D3DKMTCreateDevice
@ stub D3DKMTCreateSynchronizationObject
@ stub D3DKMTDestroyAllocation
@ stub D3DKMTDestroyContext
@ stdcall D3DKMTDestroyDevice(ptr) gdi32.D3DKMTDestroyDevice
@ stub D3DKMTDestroySynchronizationObject
@ stub D3DKMTEscape
@ stub D3DKMTGetContextSchedulingPriority
@ stub D3DKMTGetDeviceState
@ stub D3DKMTGetDisplayModeList
@ stub D3DKMTGetMultisampleMethodList
@ stub D3DKMTGetRuntimeData
@ stub D3DKMTGetSharedPrimaryHandle
@ stub D3DKMTLock
@ stdcall D3DKMTOpenAdapterFromGdiDisplayName(ptr) gdi32.D3DKMTOpenAdapterFromGdiDisplayName
@ stub D3DKMTOpenAdapterFromHdc
@ stub D3DKMTOpenResource
@ stub D3DKMTPresent
@ stdcall D3DKMTQueryAdapterInfo(ptr) gdi32.D3DKMTQueryAdapterInfo
@ stub D3DKMTQueryAllocationResidency
@ stub D3DKMTQueryResourceInfo
@ stub D3DKMTRender
@ stub D3DKMTSetAllocationPriority
@ stub D3DKMTSetContextSchedulingPriority
@ stub D3DKMTSetDisplayMode
@ stub D3DKMTSetDisplayPrivateDriverFormat
@ stub D3DKMTSetGammaRamp
@ stdcall D3DKMTSetVidPnSourceOwner(ptr) gdi32.D3DKMTSetVidPnSourceOwner
@ stub D3DKMTSignalSynchronizationObject
@ stub D3DKMTUnlock
@ stub D3DKMTWaitForSynchronizationObject
@ stub D3DKMTWaitForVerticalBlankEvent
@ stub OpenAdapter10
@ stub OpenAdapter10_2

@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_guest_D3D11CreateDevice(ptr long ptr long ptr long long ptr ptr ptr)
@ stdcall __wine_guest_D3D11CreateDeviceAndSwapChain(ptr long ptr long ptr long long ptr ptr ptr ptr ptr)
@ stdcall __wine_guest_D3D11CoreCreateDevice(ptr ptr long ptr long ptr)
@ stdcall __wine_guest_D3D11On12CreateDevice(ptr long ptr long ptr long long ptr ptr ptr)
# dxgi.dll's and d3d10core.dll's implementations live here too (see main.c);
# their own .spec files forward to these names.
@ stdcall __wine_guest_CreateDXGIFactory(ptr ptr)
@ stdcall __wine_guest_CreateDXGIFactory1(ptr ptr)
@ stdcall __wine_guest_CreateDXGIFactory2(long ptr ptr)
@ stdcall __wine_guest_DXGIGetDebugInterface1(long ptr ptr)
@ stdcall __wine_guest_DXGID3D10CreateDevice(ptr ptr ptr long ptr long ptr)
@ stdcall __wine_guest_D3D10CoreCreateDevice(ptr ptr long long ptr)
# d3d10.dll's two device-creation entry points live here too (see main.c).
@ stdcall __wine_guest_D3D10CreateDevice(ptr long ptr long long ptr)
@ stdcall __wine_guest_D3D10CreateDeviceAndSwapChain(ptr long ptr long long ptr ptr ptr)
@ stdcall __wine_dxvk_CreateDXGIFactory(ptr ptr) CreateDXGIFactory
@ stdcall __wine_dxvk_CreateDXGIFactory1(ptr ptr) CreateDXGIFactory1
@ stdcall __wine_dxvk_CreateDXGIFactory2(long ptr ptr) CreateDXGIFactory2
@ stdcall __wine_dxvk_DXGIGetDebugInterface1(long ptr ptr) DXGIGetDebugInterface1
@ stdcall __wine_dxvk_DXGIDeclareAdapterRemovalSupport() DXGIDeclareAdapterRemovalSupport
@ stdcall __wine_dxvk_DXGID3D10CreateDevice(ptr ptr ptr long ptr long ptr) DXGID3D10CreateDevice
@ stdcall __wine_dxvk_DXGID3D10RegisterLayers(ptr long) DXGID3D10RegisterLayers
@ stdcall __wine_dxvk_D3D10CoreCreateDevice(ptr ptr long long ptr) D3D10CoreCreateDevice
@ stdcall __wine_dxvk_D3D10CoreGetVersion() D3D10CoreGetVersion
@ stdcall __wine_dxvk_D3D10CoreRegisterLayers() D3D10CoreRegisterLayers
@ stdcall __wine_dxvk_D3D10CreateDevice(ptr long ptr long long ptr) D3D10CreateDevice
@ stdcall __wine_dxvk_D3D10CreateDeviceAndSwapChain(ptr long ptr long long ptr ptr ptr) D3D10CreateDeviceAndSwapChain

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr)
