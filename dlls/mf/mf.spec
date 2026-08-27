@ stub AppendPropVariant
@ stub ConvertPropVariant
@ stub CopyPropertyStore
@ stub CreateNamedPropertyStore
@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stub ExtractPropVariant
@ stdcall MFCreate3GPMediaSink(ptr ptr ptr ptr)
@ stdcall MFCreateAC3MediaSink(ptr ptr ptr)
@ stdcall MFCreateADTSMediaSink(ptr ptr ptr)
@ stub MFCreateASFByteStreamPlugin
@ stub MFCreateASFContentInfo
@ stub MFCreateASFIndexer
@ stub MFCreateASFIndexerByteStream
@ stub MFCreateASFMediaSink
@ stub MFCreateASFMediaSinkActivate
@ stub MFCreateASFMultiplexer
@ stub MFCreateASFProfile
@ stub MFCreateASFProfileFromPresentationDescriptor
@ stub MFCreateASFSplitter
@ stub MFCreateASFStreamSelector
@ stub MFCreateASFStreamingMediaSink
@ stub MFCreateASFStreamingMediaSinkActivate
@ stub MFCreateAggregateSource
@ stub MFCreateAppSourceProxy
@ stdcall MFCreateAudioRenderer(ptr ptr)
@ stdcall MFCreateAudioRendererActivate(ptr)
@ stub MFCreateByteCacheFile
@ stub MFCreateCacheManager
@ stub MFCreateCredentialCache
@ stdcall MFCreateDeviceSource(ptr ptr)
@ stub MFCreateDeviceSourceActivate
@ stub MFCreateDrmNetNDSchemePlugin
@ stub MFCreateFileBlockMap
@ stub MFCreateFileSchemePlugin
@ stdcall MFCreateFMPEG4MediaSink(ptr ptr ptr ptr)
@ stub MFCreateHttpSchemePlugin
@ stub MFCreateLPCMByteStreamPlugin
@ stub MFCreateMP3ByteStreamPlugin
@ stdcall MFCreateMP3MediaSink(ptr ptr)
@ stdcall MFCreateMPEG4MediaSink(ptr ptr ptr ptr)
@ stub MFCreateMediaProcessor
@ stdcall MFCreateMediaSession(ptr ptr)
@ stub MFCreateNSCByteStreamPlugin
@ stub MFCreateNetSchemePlugin
@ stub MFCreatePMPHost
@ stub MFCreatePMPMediaSession
@ stub MFCreatePMPServer
@ stdcall MFCreatePresentationClock(ptr)
@ stub MFCreatePresentationDescriptorFromASFProfile
@ stub MFCreateProxyLocator
@ stub MFCreateRemoteDesktopPlugin
@ stub MFCreateSAMIByteStreamPlugin
@ stdcall MFCreateSampleCopierMFT(ptr)
@ stdcall MFCreateSampleGrabberSinkActivate(ptr ptr ptr)
@ stub MFCreateSecureHttpSchemePlugin
@ stdcall MFCreateSequencerSegmentOffset(long int64 ptr)
@ stdcall MFCreateSequencerSource(ptr ptr)
@ stub MFCreateSequencerSourceRemoteStream
@ stdcall MFCreateSimpleTypeHandler(ptr)
@ stdcall MFCreateSourceResolver(ptr) mfplat.MFCreateSourceResolver
@ stdcall MFCreateStandardQualityManager(ptr)
@ stdcall MFCreateTopoLoader(ptr)
@ stdcall MFCreateTopology(ptr)
@ stdcall MFCreateTopologyNode(long ptr)
@ stub MFCreateTranscodeProfile
@ stub MFCreateTranscodeSinkActivate
@ stub MFCreateTranscodeTopology
@ stub MFCreateUrlmonSchemePlugin
@ stdcall MFCreateVideoRenderer(ptr ptr)
@ stdcall MFCreateVideoRendererActivate(long ptr)
@ stub MFCreateWMAEncoderActivate
@ stub MFCreateWMVEncoderActivate
@ stdcall MFEnumDeviceSources(ptr ptr ptr)
@ stub MFGetMultipleServiceProviders
@ stdcall MFGetService(ptr ptr ptr ptr)
@ stdcall MFGetSupportedMimeTypes(ptr)
@ stdcall MFGetSupportedSchemes(ptr)
@ stdcall MFGetTopoNodeCurrentType(ptr long long ptr)
@ stub MFReadSequencerSegmentOffset
@ stdcall MFRequireProtectedEnvironment(ptr)
@ stdcall MFShutdownObject(ptr)
@ stdcall MFTranscodeGetAudioOutputAvailableTypes(ptr long ptr ptr)
@ stub MergePropertyStore

# Media Foundation for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrappers are dlls/mf/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_MFCreateSourceResolver(ptr) mfplat.__wine_guest_MFCreateSourceResolver
@ stdcall __wine_guest_DllGetClassObject(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateAudioRendererActivate(ptr)
@ stdcall __wine_guest_MFCreatePresentationClock(ptr)
@ stdcall __wine_guest_MFCreateSampleCopierMFT(ptr)
@ stdcall __wine_guest_MFCreateSimpleTypeHandler(ptr)
@ stdcall __wine_guest_MFCreateStandardQualityManager(ptr)
@ stdcall __wine_guest_MFCreateTopoLoader(ptr)
@ stdcall __wine_guest_MFCreateTopology(ptr)
@ stdcall __wine_guest_MFCreateTopologyNode(long ptr)
@ stdcall __wine_guest_MFCreateVideoRendererActivate(long ptr)
@ stdcall __wine_guest_MFCreateSequencerSource(ptr ptr)
@ stdcall __wine_guest_MFCreateMediaSession(ptr ptr)
@ stdcall __wine_guest_MFCreateAudioRenderer(ptr ptr)
@ stdcall __wine_guest_MFCreateDeviceSource(ptr ptr)
@ stdcall __wine_guest_MFCreateMP3MediaSink(ptr ptr)
@ stdcall __wine_guest_MFCreateAC3MediaSink(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateADTSMediaSink(ptr ptr ptr)
@ stdcall __wine_guest_MFCreate3GPMediaSink(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateFMPEG4MediaSink(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFCreateMPEG4MediaSink(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFGetTopoNodeCurrentType(ptr long long ptr)
@ stdcall __wine_guest_MFTranscodeGetAudioOutputAvailableTypes(ptr long ptr ptr)
@ stdcall __wine_guest_MFEnumDeviceSources(ptr ptr ptr)
@ stdcall __wine_guest_MFGetService(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFRequireProtectedEnvironment(ptr)
@ stdcall __wine_guest_MFShutdownObject(ptr)
@ stdcall __wine_guest_MFCreateSampleGrabberSinkActivate(ptr ptr ptr)
@ stdcall __wine_guest_MFGetSupportedMimeTypes(ptr)
@ stdcall __wine_guest_MFGetSupportedSchemes(ptr)
@ stdcall __wine_guest_MFCreateSequencerSegmentOffset(long int64 ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) mfplat.__wine_com_slot_name
