# dxgi.dll -- the native ppc64le DXGI lane over DXVK.
#
# EVERY entry forwards into d3d11.dll, and that is the design rather than
# laziness: libs/winecom's proxy state is per-linkee, so a second instance
# here would mint IDXGIAdapter proxies that d3d11's instance does not
# recognise, and the first `D3D11CreateDevice(adapter, ...)` a real game makes
# would be refused as a guest-implemented object.  One instance, in d3d11,
# reached from all three modules.  dlls/d3d11/main.c holds the code.
#
# __wine_com_dispatch forwards for the same reason: ntdll's trap dispatcher
# calls it on the native namesake of whichever guest module published the
# stub, and all three must land in the same runtime.
#
# The plain names refuse for native ppc64 callers; __wine_guest_* is what the
# guest reaches (GUEST-IMPL rows in dxgi.thunks).  See dlls/d3d11/d3d11.spec.
@ stdcall CreateDXGIFactory(ptr ptr) d3d11.__wine_dxvk_CreateDXGIFactory
@ stdcall CreateDXGIFactory1(ptr ptr) d3d11.__wine_dxvk_CreateDXGIFactory1
@ stdcall CreateDXGIFactory2(long ptr ptr) d3d11.__wine_dxvk_CreateDXGIFactory2
@ stdcall DXGID3D10CreateDevice(ptr ptr ptr long ptr long ptr) d3d11.__wine_dxvk_DXGID3D10CreateDevice
@ stdcall DXGID3D10RegisterLayers(ptr long) d3d11.__wine_dxvk_DXGID3D10RegisterLayers
@ stdcall DXGIDeclareAdapterRemovalSupport() d3d11.__wine_dxvk_DXGIDeclareAdapterRemovalSupport
@ stdcall DXGIGetDebugInterface1(long ptr ptr) d3d11.__wine_dxvk_DXGIGetDebugInterface1
@ stdcall __wine_com_dispatch(long long ptr) d3d11.__wine_com_dispatch
@ stdcall __wine_com_dispatch32(long long ptr) d3d11.__wine_com_dispatch32
@ stdcall __wine_guest_CreateDXGIFactory(ptr ptr) d3d11.__wine_guest_CreateDXGIFactory
@ stdcall __wine_guest_CreateDXGIFactory1(ptr ptr) d3d11.__wine_guest_CreateDXGIFactory1
@ stdcall __wine_guest_CreateDXGIFactory2(long ptr ptr) d3d11.__wine_guest_CreateDXGIFactory2
@ stdcall __wine_guest_DXGIGetDebugInterface1(long ptr ptr) d3d11.__wine_guest_DXGIGetDebugInterface1
@ stdcall __wine_guest_DXGID3D10CreateDevice(ptr ptr ptr long ptr long ptr) d3d11.__wine_guest_DXGID3D10CreateDevice

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) d3d11.__wine_com_slot_name
@ stdcall __wine_com_slot_direct(long long ptr long) d3d11.__wine_com_slot_direct
