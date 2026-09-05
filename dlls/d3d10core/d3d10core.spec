# d3d10core.dll -- the native ppc64le D3D10 lane over DXVK's d3d10core, which
# is itself a thin layer over DXVK's d3d11.  Forwards into d3d11.dll for the
# reason dlls/dxgi/dxgi.spec gives: one winecom instance for the whole
# surface, held there.
#
# D3D10CoreGetVersion is DXVK's own export and has no Wine counterpart; it is
# here because what we promise callers should be what the implementation has.
@ stdcall D3D10CoreCreateDevice(ptr ptr long long ptr) d3d11.__wine_dxvk_D3D10CoreCreateDevice
@ stdcall D3D10CoreGetVersion() d3d11.__wine_dxvk_D3D10CoreGetVersion
@ stdcall D3D10CoreRegisterLayers() d3d11.__wine_dxvk_D3D10CoreRegisterLayers
@ stdcall __wine_com_dispatch(long long ptr) d3d11.__wine_com_dispatch
@ stdcall __wine_com_dispatch32(long long ptr) d3d11.__wine_com_dispatch32
@ stdcall __wine_guest_D3D10CoreCreateDevice(ptr ptr long long ptr) d3d11.__wine_guest_D3D10CoreCreateDevice

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) d3d11.__wine_com_slot_name
@ stdcall __wine_com_slot_direct(long long ptr long) d3d11.__wine_com_slot_direct
