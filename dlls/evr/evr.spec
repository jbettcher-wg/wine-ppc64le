@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stub MFConvertColorInfoFromDXVA
@ stdcall -import MFConvertColorInfoToDXVA(ptr ptr)
@ stub MFConvertFromFP16Array
@ stub MFConvertToFP16Array
@ stdcall -import MFCopyImage(ptr long ptr long long long)
@ stdcall -import MFCreateDXSurfaceBuffer(ptr ptr long ptr)
@ stdcall -import MFCreateVideoMediaType(ptr ptr)
@ stub MFCreateVideoMediaTypeFromBitMapInfoHeader
@ stdcall -import MFCreateVideoMediaTypeFromSubtype(ptr ptr)
@ stub MFCreateVideoMediaTypeFromVideoInfoHeader2
@ stdcall -import MFCreateVideoMediaTypeFromVideoInfoHeader(ptr long long long long int64 ptr ptr)
@ stdcall MFCreateVideoMixer(ptr ptr ptr ptr)
@ stdcall MFCreateVideoMixerAndPresenter(ptr ptr ptr ptr ptr ptr)
@ stub MFCreateVideoOTA
@ stub MFCreateVideoPresenter2
@ stdcall MFCreateVideoPresenter(ptr ptr ptr ptr)
@ stdcall MFCreateVideoSampleAllocator(ptr ptr)
@ stdcall MFCreateVideoSampleFromSurface(ptr ptr)
@ stdcall -import MFGetPlaneSize(long long long ptr)
@ stdcall -import MFGetStrideForBitmapInfoHeader(long long ptr)
@ stub MFGetUncompressedVideoFormat
@ stub MFInitVideoFormat
@ stdcall -import MFInitVideoFormat_RGB(ptr long long long)
@ stdcall MFIsFormatYUV(long)

# Media Foundation for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrappers are dlls/evr/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_DllGetClassObject(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateVideoMixer(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateVideoMixerAndPresenter(ptr ptr ptr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateVideoPresenter(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateVideoSampleAllocator(ptr ptr)
@ stdcall __wine_guest_MFCreateVideoSampleFromSurface(ptr ptr)
# ...and the three whose implementation is mfplat's, reached through evr's own
# -import forwards.  Both names MUST resolve to the same wrapper, or the
# resolver would arrive unwrapped through one of them -- exactly the second
# door dlls/mf/mf.spec closes for MFCreateSourceResolver.
@ stdcall __wine_guest_MFCreateVideoMediaType(ptr ptr) mfplat.__wine_guest_MFCreateVideoMediaType
@ stdcall __wine_guest_MFCreateVideoMediaTypeFromSubtype(ptr ptr) mfplat.__wine_guest_MFCreateVideoMediaTypeFromSubtype
@ stdcall __wine_guest_MFCreateDXSurfaceBuffer(ptr ptr long ptr) mfplat.__wine_guest_MFCreateDXSurfaceBuffer

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) mfplat.__wine_com_slot_name
