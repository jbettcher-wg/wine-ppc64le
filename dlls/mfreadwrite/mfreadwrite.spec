@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stdcall MFCreateSinkWriterFromMediaSink(ptr ptr ptr)
@ stdcall MFCreateSinkWriterFromURL(wstr ptr ptr ptr)
@ stdcall MFCreateSourceReaderFromByteStream(ptr ptr ptr)
@ stdcall MFCreateSourceReaderFromMediaSource(ptr ptr ptr)
@ stdcall MFCreateSourceReaderFromURL(wstr ptr ptr)

# Media Foundation for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrappers are dlls/mfreadwrite/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_DllGetClassObject(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateSourceReaderFromURL(wstr ptr ptr)
@ stdcall __wine_guest_MFCreateSourceReaderFromByteStream(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateSourceReaderFromMediaSource(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateSinkWriterFromURL(wstr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateSinkWriterFromMediaSink(ptr ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) mfplat.__wine_com_slot_name
