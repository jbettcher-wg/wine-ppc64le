# The native d3d12.dll is vkd3d-proton behind a unixlib (see main.c); the
# export surface is vkd3d's own d3d12.def -- what we promise callers is what
# vkd3d implements -- plus __wine_com_dispatch, the single entry ntdll's trap
# dispatcher calls to serve a guest COM vtable slot.
101 stdcall D3D12CreateDevice(ptr long ptr ptr)
102 stdcall D3D12GetDebugInterface(ptr ptr)
@ stdcall D3D12CreateRootSignatureDeserializer(ptr long ptr ptr)
@ stdcall D3D12CreateVersionedRootSignatureDeserializer(ptr long ptr ptr)
@ stdcall D3D12EnableExperimentalFeatures(long ptr ptr ptr)
@ stdcall D3D12SerializeRootSignature(ptr long ptr ptr)
@ stdcall D3D12SerializeVersionedRootSignature(ptr ptr ptr)
@ stdcall D3D12GetInterface(ptr ptr ptr)
@ stdcall __wine_com_dispatch(long long ptr)
# Cross-lane presentation: NATIVE d3d11.dll (DXVK's winecom instance) hands a
# CreateSwapChainForHwnd whose device is THIS surface's ID3D12CommandQueue
# proxy over to this lane -- winecom instances are per-linkee and cannot read
# each other's interning.  Wine-private; see main.c.
@ stdcall __wine_d3d12_create_swapchain_for_hwnd(ptr ptr ptr ptr ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr)
@ stdcall __wine_com_slot_direct(long long ptr long)
