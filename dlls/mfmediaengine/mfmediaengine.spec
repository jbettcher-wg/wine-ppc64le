@ stdcall -private DllCanUnloadNow()
@ stub DllGetActivationFactory
@ stdcall -private DllGetClassObject(ptr ptr ptr)

# Media Foundation for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrapper is dlls/mfmediaengine/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_DllGetClassObject(ptr ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) mfplat.__wine_com_slot_name
