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
