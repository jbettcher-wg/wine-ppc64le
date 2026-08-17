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
