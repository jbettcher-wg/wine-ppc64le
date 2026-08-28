# d3d10.dll -- the native ppc64le D3D10 front end.
#
# ONLY DEVICE CREATION IS SERVED, and the rest is refused by name in
# d3d10.thunks.  DXVK ships no d3d10.dll: upstream removed it and kept
# d3d10core, which is a thin layer over its own d3d11 and is what this lane
# serves (dlls/d3d10core).  Everything else this module exports -- the effects
# framework, the state-block helpers, the shader reflection -- is WINE'S OWN
# implementation, written in C against ID3D10Device.  On this lane an
# ID3D10Device is a guest proxy whose vtable is an array of x86-64 trap stubs,
# so a native ppc64 effects framework driving one would execute those bytes as
# ppc64 on its first call.  There is no way to serve those without either a
# reverse-proxy runtime or a native D3D10 effects implementation, and refusing
# by name is the honest answer until one exists.
#
# The two that ARE served forward into d3d11.dll, which owns the single winecom
# instance for the whole D3D11/DXGI/D3D10 surface -- see dlls/dxgi/dxgi.spec
# for why there can only be one.
@ stdcall D3D10CompileEffectFromMemory(ptr long str ptr ptr long long ptr ptr)
@ stdcall D3D10CompileShader(ptr long str ptr ptr str str long ptr ptr)
@ stdcall D3D10CreateBlob(long ptr) d3dcompiler_43.D3DCreateBlob
@ stdcall D3D10CreateDevice(ptr long ptr long long ptr) d3d11.__wine_dxvk_D3D10CreateDevice
@ stdcall D3D10CreateDeviceAndSwapChain(ptr long ptr long long ptr ptr ptr) d3d11.__wine_dxvk_D3D10CreateDeviceAndSwapChain
@ stdcall D3D10CreateEffectFromMemory(ptr long long ptr ptr ptr)
@ stdcall D3D10CreateEffectPoolFromMemory(ptr long long ptr ptr)
@ stdcall D3D10CreateStateBlock(ptr ptr ptr)
@ stub D3D10DisassembleEffect
@ stdcall D3D10DisassembleShader(ptr long long ptr ptr)
@ stdcall D3D10GetGeometryShaderProfile(ptr)
@ stdcall D3D10GetInputAndOutputSignatureBlob(ptr long ptr) d3dcompiler_43.D3DGetInputAndOutputSignatureBlob
@ stdcall D3D10GetInputSignatureBlob(ptr long ptr) d3dcompiler_43.D3DGetInputSignatureBlob
@ stdcall D3D10GetOutputSignatureBlob(ptr long ptr) d3dcompiler_43.D3DGetOutputSignatureBlob
@ stdcall D3D10GetPixelShaderProfile(ptr)
@ stdcall D3D10GetShaderDebugInfo(ptr long ptr) d3dcompiler_43.D3DGetDebugInfo
@ stub D3D10GetVersion
@ stdcall D3D10GetVertexShaderProfile(ptr)
@ stub D3D10PreprocessShader
@ stdcall D3D10ReflectShader(ptr long ptr)
@ stub D3D10RegisterLayers
@ stdcall D3D10StateBlockMaskDifference(ptr ptr ptr)
@ stdcall D3D10StateBlockMaskDisableAll(ptr)
@ stdcall D3D10StateBlockMaskDisableCapture(ptr long long long)
@ stdcall D3D10StateBlockMaskEnableAll(ptr)
@ stdcall D3D10StateBlockMaskEnableCapture(ptr long long long)
@ stdcall D3D10StateBlockMaskGetSetting(ptr long long)
@ stdcall D3D10StateBlockMaskIntersect(ptr ptr ptr)
@ stdcall D3D10StateBlockMaskUnion(ptr ptr ptr)

@ stdcall __wine_com_dispatch(long long ptr) d3d11.__wine_com_dispatch
@ stdcall __wine_com_dispatch32(long long ptr) d3d11.__wine_com_dispatch32
@ stdcall __wine_com_refuse() combase.__wine_com_refuse
@ stdcall __wine_guest_D3D10CreateDevice(ptr long ptr long long ptr) d3d11.__wine_guest_D3D10CreateDevice
@ stdcall __wine_guest_D3D10CreateDeviceAndSwapChain(ptr long ptr long long ptr ptr ptr) d3d11.__wine_guest_D3D10CreateDeviceAndSwapChain

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) d3d11.__wine_com_slot_name
