@ stub FormatTagFromWfx
@ stub MFCreateGuid
@ stub MFGetIoPortHandle
@ stub MFGetPlatformVersion
@ stub MFGetRandomNumber
@ stub MFIsFeatureEnabled
@ stub MFIsQueueThread
@ stub MFPlatformBigEndian
@ stub MFPlatformLittleEndian
@ stub ValidateWaveFormat
@ stub CopyPropVariant
@ stub CreatePropVariant
@ stdcall CreatePropertyStore(ptr)
@ stub DestroyPropVariant
@ stub GetAMSubtypeFromD3DFormat
@ stub GetD3DFormatFromMFSubtype
@ stub LFGetGlobalPool
@ stdcall MFAddPeriodicCallback(ptr ptr ptr) rtworkq.RtwqAddPeriodicCallback
@ stdcall MFAllocateSerialWorkQueue(long ptr) rtworkq.RtwqAllocateSerialWorkQueue
@ stdcall MFAllocateWorkQueue(ptr)
@ stdcall MFAllocateWorkQueueEx(long ptr) rtworkq.RtwqAllocateWorkQueue
@ stub MFAppendCollection
@ stdcall MFAverageTimePerFrameToFrameRate(int64 ptr ptr)
@ stdcall MFBeginCreateFile(long long long wstr ptr ptr ptr)
@ stub MFBeginGetHostByName
@ stdcall MFBeginRegisterWorkQueueWithMMCSS(long wstr long ptr ptr)
@ stdcall MFBeginRegisterWorkQueueWithMMCSSEx(long wstr long long ptr ptr) rtworkq.RtwqBeginRegisterWorkQueueWithMMCSS
@ stdcall MFBeginUnregisterWorkQueueWithMMCSS(long ptr ptr) rtworkq.RtwqBeginUnregisterWorkQueueWithMMCSS
@ stub MFBlockThread
@ stub MFCalculateBitmapImageSize
@ stdcall MFCalculateImageSize(ptr long long ptr)
@ stdcall MFCancelCreateFile(ptr)
@ stdcall MFCancelWorkItem(int64) rtworkq.RtwqCancelWorkItem
@ stdcall MFCompareFullToPartialMediaType(ptr ptr)
@ stub MFCompareSockaddrAddresses
@ stub MFConvertColorInfoFromDXVA
@ stdcall MFConvertColorInfoToDXVA(ptr ptr)
@ stub MFConvertFromFP16Array
@ stub MFConvertToFP16Array
@ stdcall MFCopyImage(ptr long ptr long long long)
@ stdcall MFCreate2DMediaBuffer(long long long long ptr)
@ stdcall MFCreateAMMediaTypeFromMFMediaType(ptr int128 ptr)
@ stdcall MFCreateAlignedMemoryBuffer(long long ptr)
@ stdcall MFCreateAsyncResult(ptr ptr ptr ptr) rtworkq.RtwqCreateAsyncResult
@ stdcall MFCreateAttributes(ptr long)
@ stdcall MFCreateAudioMediaType(ptr ptr)
@ stdcall MFCreateCollection(ptr)
@ stdcall MFCreateD3D12SynchronizationObject(ptr ptr ptr)
@ stdcall MFCreateDXGIDeviceManager(ptr ptr)
@ stdcall MFCreateDXGISurfaceBuffer(ptr ptr long long ptr)
@ stdcall MFCreateDXSurfaceBuffer(ptr ptr long ptr)
@ stdcall MFCreateEventQueue(ptr)
@ stdcall MFCreateFile(long long long wstr ptr)
@ stdcall MFCreateLegacyMediaBufferOnMFMediaBuffer(ptr ptr long ptr)
@ stdcall MFCreateMFByteStreamOnStream(ptr ptr)
@ stdcall MFCreateMFByteStreamOnStreamEx(ptr ptr)
@ stdcall MFCreateMFByteStreamWrapper(ptr ptr)
@ stdcall MFCreateMFVideoFormatFromMFMediaType(ptr ptr ptr)
@ stdcall MFCreateMediaBufferFromMediaType(ptr int64 long long ptr)
@ stub MFCreateMediaBufferWrapper
@ stdcall MFCreateMediaEvent(long ptr long ptr ptr)
@ stdcall MFCreateMediaType(ptr)
@ stdcall MFCreateMediaTypeFromRepresentation(int128 ptr ptr)
@ stdcall MFCreateMemoryBuffer(long ptr)
@ stub MFCreateMemoryStream
@ stdcall MFCreatePathFromURL(wstr ptr)
@ stdcall MFCreatePresentationDescriptor(long ptr ptr)
@ stdcall MFCreateSample(ptr)
@ stub MFCreateSocket
@ stub MFCreateSocketListener
@ stdcall MFCreateSourceResolver(ptr)
@ stdcall MFCreateStreamDescriptor(long long ptr ptr)
@ stdcall MFCreateSystemTimeSource(ptr)
@ stub MFCreateSystemUnderlyingClock
@ stdcall MFCreateTempFile(long long long ptr)
@ stdcall MFCreateTrackedSample(ptr)
@ stdcall MFCreateTransformActivate(ptr)
@ stub MFCreateURLFromPath
@ stub MFCreateUdpSockets
@ stdcall MFCreateVideoMediaType(ptr ptr)
@ stub MFCreateVideoMediaTypeFromBitMapInfoHeader
@ stub MFCreateVideoMediaTypeFromBitMapInfoHeaderEx
@ stdcall MFCreateVideoMediaTypeFromSubtype(ptr ptr)
@ stub MFCreateVideoMediaTypeFromVideoInfoHeader2
@ stdcall MFCreateVideoMediaTypeFromVideoInfoHeader(ptr long long long long int64 ptr ptr)
@ stdcall MFCreateVideoSampleAllocatorEx(ptr ptr)
@ stdcall MFCreateWaveFormatExFromMFMediaType(ptr ptr ptr long)
@ stdcall MFDeserializeAttributesFromStream(ptr long ptr)
@ stub MFDeserializeEvent
@ stub MFDeserializeMediaTypeFromStream
@ stub MFDeserializePresentationDescriptor
@ stdcall MFEndCreateFile(ptr ptr)
@ stub MFEndGetHostByName
@ stdcall MFEndRegisterWorkQueueWithMMCSS(ptr ptr) rtworkq.RtwqEndRegisterWorkQueueWithMMCSS
@ stdcall MFEndUnregisterWorkQueueWithMMCSS(ptr) rtworkq.RtwqEndUnregisterWorkQueueWithMMCSS
@ stdcall MFFrameRateToAverageTimePerFrame(long long ptr)
@ stub MFFreeAdaptersAddresses
@ stub MFGetAdaptersAddresses
@ stdcall MFGetAttributesAsBlob(ptr ptr long)
@ stdcall MFGetAttributesAsBlobSize(ptr ptr)
@ stub MFGetConfigurationDWORD
@ stub MFGetConfigurationPolicy
@ stub MFGetConfigurationStore
@ stub MFGetConfigurationString
@ stub MFGetMFTMerit
@ stub MFGetNumericNameFromSockaddr
@ stdcall MFGetPlaneSize(long long long ptr)
@ stub MFGetPlatform
@ stdcall MFGetPluginControl(ptr)
@ stub MFGetPrivateWorkqueues
@ stub MFGetSockaddrFromNumericName
@ stdcall MFGetStrideForBitmapInfoHeader(long long ptr)
@ stdcall MFGetSystemTime()
@ stdcall MFGetTimerPeriodicity(ptr)
@ stub MFGetUncompressedVideoFormat
@ stdcall MFGetWorkQueueMMCSSClass(long ptr ptr) rtworkq.RtwqGetWorkQueueMMCSSClass
@ stdcall MFGetWorkQueueMMCSSTaskId(long ptr) rtworkq.RtwqGetWorkQueueMMCSSTaskId
@ stdcall MFGetWorkQueueMMCSSPriority(long ptr) rtworkq.RtwqGetWorkQueueMMCSSPriority
@ stdcall MFHeapAlloc(long long str long long)
@ stdcall MFHeapFree(ptr)
@ stdcall MFInitAMMediaTypeFromMFMediaType(ptr int128 ptr)
@ stdcall MFInitAttributesFromBlob(ptr ptr long)
@ stdcall MFInitMediaTypeFromAMMediaType(ptr ptr)
@ stdcall MFInitMediaTypeFromMFVideoFormat(ptr ptr long)
@ stdcall MFInitMediaTypeFromMPEG1VideoInfo(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromMPEG2VideoInfo(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromVideoInfoHeader2(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromVideoInfoHeader(ptr ptr long ptr)
@ stdcall MFInitMediaTypeFromWaveFormatEx(ptr ptr long)
@ stub MFInitVideoFormat
@ stdcall MFInitVideoFormat_RGB(ptr long long long)
@ stdcall MFInvokeCallback(ptr)
@ stub MFJoinIoPort
@ stdcall MFJoinWorkQueue(long long ptr) rtworkq.RtwqJoinWorkQueue
@ stdcall MFLockDXGIDeviceManager(ptr ptr)
@ stdcall MFLockPlatform() rtworkq.RtwqLockPlatform
@ stdcall MFLockSharedWorkQueue(wstr long ptr ptr) rtworkq.RtwqLockSharedWorkQueue
@ stdcall MFLockWorkQueue(long) rtworkq.RtwqLockWorkQueue
@ stdcall MFMapDX9FormatToDXGIFormat(long)
@ stdcall MFMapDXGIFormatToDX9Format(long)
@ stdcall MFPutWaitingWorkItem(long long ptr ptr) rtworkq.RtwqPutWaitingWorkItem
@ stdcall MFPutWorkItem(long ptr ptr)
@ stdcall MFPutWorkItem2(long long ptr ptr)
@ stdcall MFPutWorkItemEx(long ptr)
@ stdcall MFPutWorkItemEx2(long long ptr)
@ stub MFRecordError
@ stdcall MFRegisterLocalByteStreamHandler(wstr wstr ptr)
@ stdcall MFRegisterLocalSchemeHandler(wstr ptr)
@ stdcall MFRegisterPlatformWithMMCSS(wstr ptr long) rtworkq.RtwqRegisterPlatformWithMMCSS
@ stdcall MFRemovePeriodicCallback(long) rtworkq.RtwqRemovePeriodicCallback
@ stdcall MFScheduleWorkItem(ptr ptr int64 ptr)
@ stdcall MFScheduleWorkItemEx(ptr int64 ptr) rtworkq.RtwqScheduleWorkItem
@ stdcall MFSerializeAttributesToStream(ptr long ptr)
@ stub MFSerializeEvent
@ stub MFSerializeMediaTypeToStream
@ stub MFSerializePresentationDescriptor
@ stub MFSetSockaddrAny
@ stdcall MFShutdown()
@ stdcall MFStartup(long long)
@ stub MFStreamDescriptorProtectMediaType
@ stdcall MFTEnum(int128 long ptr ptr ptr ptr ptr)
@ stdcall MFTEnum2(int128 long ptr ptr ptr ptr ptr)
@ stdcall MFTEnumEx(int128 long ptr ptr ptr ptr)
@ stdcall MFTGetInfo(int128 ptr ptr ptr ptr ptr ptr)
@ stdcall MFTRegister(int128 int128 wstr long long ptr long ptr ptr)
@ stdcall MFTRegisterLocal(ptr ptr wstr long long ptr long ptr)
@ stdcall MFTRegisterLocalByCLSID(ptr ptr wstr long long ptr long ptr)
@ stdcall MFTUnregister(int128)
@ stdcall MFTUnregisterLocal(ptr)
@ stdcall MFTUnregisterLocalByCLSID(int128)
@ stdcall MFUnregisterPlatformFromMMCSS() rtworkq.RtwqUnregisterPlatformFromMMCSS
@ stub MFTraceError
@ stub MFTraceFuncEnter
@ stub MFUnblockThread
@ stdcall MFUnjoinWorkQueue(long long) rtworkq.RtwqUnjoinWorkQueue
@ stdcall MFUnlockDXGIDeviceManager()
@ stdcall MFUnlockPlatform() rtworkq.RtwqUnlockPlatform
@ stdcall MFUnlockWorkQueue(long) rtworkq.RtwqUnlockWorkQueue
@ stdcall MFUnwrapMediaType(ptr ptr)
@ stub MFValidateMediaTypeSize
@ stdcall MFWrapMediaType(ptr ptr ptr ptr)
@ stdcall -ret64 MFllMulDiv(int64 int64 int64 int64)
@ stub PropVariantFromStream
@ stub PropVariantToStream

# Media Foundation for x86-64 guests (ppc64le/mf/README.md): the ONE winecom
# runtime instance for the MF surface lives here; mf.dll and mfreadwrite.dll
# reach it through these exports, and spec2thunk's GUEST-IMPL redirect points
# each guest export's native resolution at its __wine_guest_* wrapper.
@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_com_wrap(ptr long)
@ stdcall __wine_com_unwrap(ptr)
@ stdcall __wine_com_translate_in(ptr ptr)
@ stdcall __wine_com_wrap_out_iface(long ptr ptr)
@ stdcall __wine_com_wrap_static(ptr long)
@ stdcall __wine_com_iface_from_iid(ptr)
@ stdcall __wine_mf_translate_in(ptr ptr ptr ptr ptr)
@ stdcall __wine_mf_translate_in_iface(ptr ptr ptr long ptr ptr)
@ stdcall __wine_mf_translate_in_end(ptr)
# The reverse-proxy mechanism gate's only entry point; see
# include/wine/winecom_selftest.h.  Nothing in Wine calls it.
@ stdcall __wine_winecom_reverse_selftest(ptr ptr ptr)
@ stdcall __wine_guest___wine_winecom_reverse_selftest(ptr ptr ptr)
@ stdcall __wine_winecom_reverse_nest(ptr ptr)
@ stdcall __wine_guest___wine_winecom_reverse_nest(ptr ptr)
@ stdcall __wine_mf_refuse_cross_surface(ptr ptr ptr ptr)
@ stdcall __wine_mf_audit_propvariant_out(ptr ptr long ptr)
@ stdcall __wine_guest_MFCreateAttributes(ptr long)
@ stdcall __wine_guest_MFCreateMediaType(ptr)
@ stdcall __wine_guest_MFCreateSample(ptr)
@ stdcall __wine_guest_MFCreateMemoryBuffer(long ptr)
@ stdcall __wine_guest_MFCreateAlignedMemoryBuffer(long long ptr)
@ stdcall __wine_guest_MFCreate2DMediaBuffer(long long long long ptr)
@ stdcall __wine_guest_MFCreateCollection(ptr)
@ stdcall __wine_guest_MFCreateEventQueue(ptr)
@ stdcall __wine_guest_MFCreateSourceResolver(ptr)
@ stdcall __wine_guest_MFCreateFile(long long long wstr ptr)
@ stdcall __wine_guest_MFCreateTempFile(long long long ptr)
@ stdcall __wine_guest_MFCreateSystemTimeSource(ptr)
@ stdcall __wine_guest_MFCreateTrackedSample(ptr)
@ stdcall __wine_guest_MFCreateAudioMediaType(ptr ptr)
@ stdcall __wine_guest_MFCreateVideoMediaType(ptr ptr)
@ stdcall __wine_guest_MFCreateVideoMediaTypeFromSubtype(ptr ptr)
@ stdcall __wine_guest_MFCreateTransformActivate(ptr)
@ stdcall __wine_guest_MFGetPluginControl(ptr)
@ stdcall __wine_guest_MFCreateVideoSampleAllocatorEx(ptr ptr)
@ stdcall __wine_guest_MFCreateMediaBufferFromMediaType(ptr int64 long long ptr)
@ stdcall __wine_guest_MFWrapMediaType(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFUnwrapMediaType(ptr ptr)
@ stdcall __wine_guest_MFCreateStreamDescriptor(long long ptr ptr)
@ stdcall __wine_guest_MFCreatePresentationDescriptor(long ptr ptr)
@ stdcall __wine_guest_MFCompareFullToPartialMediaType(ptr ptr)
@ stdcall __wine_guest_MFCreateMFVideoFormatFromMFMediaType(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateWaveFormatExFromMFMediaType(ptr ptr ptr long)
@ stdcall __wine_guest_MFInitMediaTypeFromWaveFormatEx(ptr ptr long)
@ stdcall __wine_guest_MFInitMediaTypeFromMFVideoFormat(ptr ptr long)
@ stdcall __wine_guest_MFInitMediaTypeFromAMMediaType(ptr ptr)
@ stdcall __wine_guest_MFInitMediaTypeFromVideoInfoHeader(ptr ptr long ptr)
@ stdcall __wine_guest_MFInitMediaTypeFromVideoInfoHeader2(ptr ptr long ptr)
@ stdcall __wine_guest_MFInitMediaTypeFromMPEG1VideoInfo(ptr ptr long ptr)
@ stdcall __wine_guest_MFInitMediaTypeFromMPEG2VideoInfo(ptr ptr long ptr)
@ stdcall __wine_guest_MFGetAttributesAsBlob(ptr ptr long)
@ stdcall __wine_guest_MFGetAttributesAsBlobSize(ptr ptr)
@ stdcall __wine_guest_MFInitAttributesFromBlob(ptr ptr long)
@ stdcall __wine_guest_MFRegisterLocalByteStreamHandler(wstr wstr ptr)
@ stdcall __wine_guest_MFRegisterLocalSchemeHandler(wstr ptr)
@ stdcall __wine_guest_MFCreateAsyncResult(ptr ptr ptr ptr)
@ stdcall __wine_guest_MFInvokeCallback(ptr)
@ stdcall __wine_guest_MFTRegisterLocal(ptr ptr wstr long long ptr long ptr)
@ stdcall __wine_guest_MFTUnregisterLocal(ptr)
@ stdcall __wine_guest_MFPutWorkItem(long ptr ptr)
@ stdcall __wine_guest_MFPutWorkItem2(long long ptr ptr)
@ stdcall __wine_guest_MFPutWorkItemEx(long ptr)
@ stdcall __wine_guest_MFPutWorkItemEx2(long long ptr)
@ stdcall __wine_guest_MFPutWaitingWorkItem(ptr long ptr ptr)
@ stdcall __wine_guest_MFScheduleWorkItem(ptr ptr int64 ptr)
@ stdcall __wine_guest_MFScheduleWorkItemEx(ptr int64 ptr)
@ stdcall __wine_guest_MFBeginCreateFile(long long long wstr ptr ptr ptr)
@ stdcall __wine_guest_MFEndCreateFile(ptr ptr)
@ stdcall __wine_guest_MFBeginRegisterWorkQueueWithMMCSS(long wstr long ptr ptr)
@ stdcall __wine_guest_MFBeginRegisterWorkQueueWithMMCSSEx(long wstr long long ptr ptr)
@ stdcall __wine_guest_MFEndRegisterWorkQueueWithMMCSS(ptr ptr)
@ stdcall __wine_guest_MFBeginUnregisterWorkQueueWithMMCSS(long ptr ptr)
@ stdcall __wine_guest_MFEndUnregisterWorkQueueWithMMCSS(ptr)
@ stdcall __wine_guest_MFCancelCreateFile(ptr)
@ stdcall __wine_guest_MFAddPeriodicCallback(ptr ptr ptr)
@ stdcall __wine_guest_MFCreateMFByteStreamOnStream(ptr ptr)
@ stdcall __wine_guest_MFCreateMFByteStreamOnStreamEx(ptr ptr)
@ stdcall __wine_guest_MFSerializeAttributesToStream(ptr long ptr)
@ stdcall __wine_guest_MFDeserializeAttributesFromStream(ptr long ptr)
@ stdcall __wine_guest_MFCreateDXGIDeviceManager(ptr ptr)
@ stdcall __wine_guest_MFLockDXGIDeviceManager(ptr ptr)
@ stdcall __wine_guest_MFCreateDXGISurfaceBuffer(ptr ptr long long ptr)
@ stdcall __wine_guest_MFCreateDXSurfaceBuffer(ptr ptr long ptr)
@ stdcall __wine_guest_MFCreateLegacyMediaBufferOnMFMediaBuffer(ptr ptr long ptr)
@ stdcall __wine_guest_MFCreateMediaEvent(long ptr long ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr)
