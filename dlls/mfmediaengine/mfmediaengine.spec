@ stdcall -private DllCanUnloadNow()
@ stub DllGetActivationFactory
@ stdcall -private DllGetClassObject(ptr ptr ptr)

# Media Foundation for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrapper is dlls/mfmediaengine/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_DllGetClassObject(ptr ptr ptr)
