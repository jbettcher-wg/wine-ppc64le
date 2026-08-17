/* GENERATED -- do not edit.
 *
 * Marshal tables for the wine-syscom surface (70 interfaces, 805 vtable
 * slots).  Interface order is sorted by name -- the same order spec2thunk
 * COM mode gives the guest module's stub arrays, and the runtime
 * cross-checks the IIDs at attach so the two cannot silently disagree.
 * Slot/iface types and WINECOM_CA_* classes come from
 * include/wine/winecom.h, which must be included before this file.
 *
 * The 12 audio interfaces -- the XAudio2 2.7 family and the WASAPI device
 * chain -- are generated from interfaces_syscom.json by
 * ppc64le/syscom/gen_syscom_audio.py, which also owns the enum, the
 * interface array and the roster indices in every xaux[] here.  The other
 * 58 rows are the emitted output of the earlier generator named in the
 * git history, reused verbatim; ppc64le/syscom/check-syscom-audio.sh
 * proves that reuse is byte-exact.
 *
 * WHICH IS WHY THE ROWS ARE NOT ALL THE SAME LENGTH, and it is a
 * statement rather than an oversight: this generator's rows carry the
 * fpmask/fpwide/xmask the REVERSE direction reads, and the reused rows
 * stop at aux2 because the generator that wrote them predates those
 * members.  C zero-fills the rest, which is the right value for a row
 * whose float parameters were never classified and whose interface
 * IN-parameters this port declines to reverse-proxy -- but a reader must
 * not take a 0 on a reused row as a measurement.
 *
 * The reused rows' missing xmask is a POLICY as well as an accident.
 * gen_syscom_audio.py re-derives every one of them from the roster's own
 * parameter text and cross-checks it against the block as emitted, so the
 * information is present and checked; the bit is then withheld, because
 * every reused interface is IUnknown-derived and a reverse proxy of one
 * can be QueryInterface'd into any other -- including the DirectMusic and
 * moniker tables that refuse in both directions.  --report names every
 * withheld parameter.
 */

enum syscom_iface_index
{
    SYSCOM_IFACE_IActivationFactory = 0,
    SYSCOM_IFACE_IAgileObject = 1,
    SYSCOM_IFACE_IAudioClient = 2,
    SYSCOM_IFACE_IAudioRenderClient = 3,
    SYSCOM_IFACE_IBindCtx = 4,
    SYSCOM_IFACE_IClassFactory = 5,
    SYSCOM_IFACE_IConnectionPoint = 6,
    SYSCOM_IFACE_IConnectionPointContainer = 7,
    SYSCOM_IFACE_ICreateErrorInfo = 8,
    SYSCOM_IFACE_IDirectMusic = 9,
    SYSCOM_IFACE_IDirectMusicAudioPath = 10,
    SYSCOM_IFACE_IDirectMusicBand = 11,
    SYSCOM_IFACE_IDirectMusicBuffer = 12,
    SYSCOM_IFACE_IDirectMusicDownloadedInstrument = 13,
    SYSCOM_IFACE_IDirectMusicGetLoader = 14,
    SYSCOM_IFACE_IDirectMusicGraph = 15,
    SYSCOM_IFACE_IDirectMusicInstrument = 16,
    SYSCOM_IFACE_IDirectMusicLoader = 17,
    SYSCOM_IFACE_IDirectMusicLoader8 = 18,
    SYSCOM_IFACE_IDirectMusicObject = 19,
    SYSCOM_IFACE_IDirectMusicPerformance = 20,
    SYSCOM_IFACE_IDirectMusicPerformance8 = 21,
    SYSCOM_IFACE_IDirectMusicPort = 22,
    SYSCOM_IFACE_IDirectMusicSegment = 23,
    SYSCOM_IFACE_IDirectMusicSegment8 = 24,
    SYSCOM_IFACE_IDirectMusicSegmentState = 25,
    SYSCOM_IFACE_IDirectMusicTool = 26,
    SYSCOM_IFACE_IDirectMusicTrack = 27,
    SYSCOM_IFACE_IDirectSound = 28,
    SYSCOM_IFACE_IDirectSoundBuffer = 29,
    SYSCOM_IFACE_IDispatch = 30,
    SYSCOM_IFACE_IEnumConnectionPoints = 31,
    SYSCOM_IFACE_IEnumConnections = 32,
    SYSCOM_IFACE_IEnumMoniker = 33,
    SYSCOM_IFACE_IEnumSTATSTG = 34,
    SYSCOM_IFACE_IEnumString = 35,
    SYSCOM_IFACE_IEnumUnknown = 36,
    SYSCOM_IFACE_IErrorInfo = 37,
    SYSCOM_IFACE_IGlobalInterfaceTable = 38,
    SYSCOM_IFACE_IInspectable = 39,
    SYSCOM_IFACE_ILockBytes = 40,
    SYSCOM_IFACE_IMMDevice = 41,
    SYSCOM_IFACE_IMMDeviceCollection = 42,
    SYSCOM_IFACE_IMMDeviceEnumerator = 43,
    SYSCOM_IFACE_IMMNotificationClient = 44,
    SYSCOM_IFACE_IMalloc = 45,
    SYSCOM_IFACE_IMarshal = 46,
    SYSCOM_IFACE_IMoniker = 47,
    SYSCOM_IFACE_IMultiQI = 48,
    SYSCOM_IFACE_IPersist = 49,
    SYSCOM_IFACE_IPersistFile = 50,
    SYSCOM_IFACE_IPersistStream = 51,
    SYSCOM_IFACE_IPersistStreamInit = 52,
    SYSCOM_IFACE_IRecordInfo = 53,
    SYSCOM_IFACE_IReferenceClock = 54,
    SYSCOM_IFACE_IRunningObjectTable = 55,
    SYSCOM_IFACE_ISequentialStream = 56,
    SYSCOM_IFACE_IStorage = 57,
    SYSCOM_IFACE_IStream = 58,
    SYSCOM_IFACE_ISupportErrorInfo = 59,
    SYSCOM_IFACE_ITypeComp = 60,
    SYSCOM_IFACE_ITypeInfo = 61,
    SYSCOM_IFACE_ITypeLib = 62,
    SYSCOM_IFACE_IUnknown = 63,
    SYSCOM_IFACE_IXAudio2 = 64,
    SYSCOM_IFACE_IXAudio2EngineCallback = 65,
    SYSCOM_IFACE_IXAudio2MasteringVoice = 66,
    SYSCOM_IFACE_IXAudio2SourceVoice = 67,
    SYSCOM_IFACE_IXAudio2SubmixVoice = 68,
    SYSCOM_IFACE_IXAudio2Voice = 69,
    SYSCOM_IFACE_COUNT = 70
};

#define SYSCOM_HAND_COUNT 9

#define SYSCOM_SLOT_IXAudio2Voice_DestroyVoice 18

/* hand_funcs[] order in dlls/combase/syscom.c:
 *   0 hand_create_source_voice
 *   1 hand_create_submix_voice
 *   2 hand_create_mastering_voice
 *   3 hand_set_output_voices
 *   4 hand_set_effect_chain
 *   5 hand_mmdevice_activate
 *   6 hand_mmdev_register_notify
 *   7 hand_mmdev_unregister_notify
 *   8 hand_f_i
 */

static const unsigned char cls_IActivationFactory_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IActivationFactory_6[] = { 39 };
static const struct winecom_slot slots_IActivationFactory[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IInspectable::GetIids", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IInspectable::GetRuntimeClassName", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IInspectable::GetTrustLevel", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IActivationFactory::ActivateInstance", NULL, cls_IActivationFactory_6, xaux_IActivationFactory_6, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IAgileObject[3] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
};

static const unsigned char cls_IAudioClient_14[] = { WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const struct winecom_slot slots_IAudioClient[15] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IAudioClient::Initialize", NULL, NULL, NULL, 7, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetBufferSize", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetStreamLatency", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetCurrentPadding", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::IsFormatSupported", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetMixFormat", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetDevicePeriod", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::Start", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::Stop", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::Reset", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::SetEventHandle", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioClient::GetService", NULL, cls_IAudioClient_14, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const struct winecom_slot slots_IAudioRenderClient[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IAudioRenderClient::GetBuffer", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IAudioRenderClient::ReleaseBuffer", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IBindCtx_3[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IBindCtx_4[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IBindCtx_8[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IBindCtx_8[] = { 55 };
static const unsigned char cls_IBindCtx_11[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IBindCtx_11[] = { 35 };
static const struct winecom_slot slots_IBindCtx[13] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IBindCtx::RegisterObjectBound", NULL, cls_IBindCtx_3, NULL, 2, 0, 0, 0 },
    { "IBindCtx::RevokeObjectBound", NULL, cls_IBindCtx_4, NULL, 2, 0, 0, 0 },
    { "IBindCtx::ReleaseBoundObjects", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IBindCtx::SetBindOptions", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IBindCtx::GetBindOptions", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IBindCtx::GetRunningObjectTable", NULL, cls_IBindCtx_8, xaux_IBindCtx_8, 2, 0, 0, 0 },
    { "IBindCtx::RegisterObjectParam",
      "IBindCtx::RegisterObjectParam: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IBindCtx::GetObjectParam",
      "IBindCtx::GetObjectParam: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IBindCtx::EnumObjectParam", NULL, cls_IBindCtx_11, xaux_IBindCtx_11, 2, 0, 0, 0 },
    { "IBindCtx::RevokeObjectParam",
      "IBindCtx::RevokeObjectParam: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IClassFactory_3[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const struct winecom_slot slots_IClassFactory[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IClassFactory::CreateInstance", NULL, cls_IClassFactory_3, NULL, 4, 0, 1, 0 },
    { "IClassFactory::LockServer", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IConnectionPoint_4[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IConnectionPoint_4[] = { 7 };
static const unsigned char cls_IConnectionPoint_5[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IConnectionPoint_7[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IConnectionPoint_7[] = { 32 };
static const struct winecom_slot slots_IConnectionPoint[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IConnectionPoint::GetConnectionInterface", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IConnectionPoint::GetConnectionPointContainer", NULL, cls_IConnectionPoint_4, xaux_IConnectionPoint_4, 2, 0, 0, 0 },
    { "IConnectionPoint::Advise", NULL, cls_IConnectionPoint_5, NULL, 3, 0, 0, 0 },
    { "IConnectionPoint::Unadvise", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IConnectionPoint::EnumConnections", NULL, cls_IConnectionPoint_7, xaux_IConnectionPoint_7, 2, 0, 0, 0 },
};

static const unsigned char cls_IConnectionPointContainer_3[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IConnectionPointContainer_3[] = { 31 };
static const unsigned char cls_IConnectionPointContainer_4[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IConnectionPointContainer_4[] = { 0, 6 };
static const struct winecom_slot slots_IConnectionPointContainer[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IConnectionPointContainer::EnumConnectionPoints", NULL, cls_IConnectionPointContainer_3, xaux_IConnectionPointContainer_3, 2, 0, 0, 0 },
    { "IConnectionPointContainer::FindConnectionPoint", NULL, cls_IConnectionPointContainer_4, xaux_IConnectionPointContainer_4, 3, 0, 0, 0 },
};

static const struct winecom_slot slots_ICreateErrorInfo[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ICreateErrorInfo::SetGUID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "ICreateErrorInfo::SetSource",
      "ICreateErrorInfo::SetSource: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "ICreateErrorInfo::SetDescription",
      "ICreateErrorInfo::SetDescription: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "ICreateErrorInfo::SetHelpFile",
      "ICreateErrorInfo::SetHelpFile: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "ICreateErrorInfo::SetHelpContext", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusic_4[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN };
static const unsigned char xaux_IDirectMusic_4[] = { 0, 12, 0 };
static const unsigned char cls_IDirectMusic_5[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN };
static const unsigned char xaux_IDirectMusic_5[] = { 0, 0, 22, 0 };
static const unsigned char cls_IDirectMusic_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const struct winecom_slot slots_IDirectMusic[12] =
{
    { "IDirectMusic::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusic::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusic::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusic::EnumPort", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusic::CreateMusicBuffer", NULL, cls_IDirectMusic_4, xaux_IDirectMusic_4, 4, 0, 0, 0 },
    { "IDirectMusic::CreatePort", NULL, cls_IDirectMusic_5, xaux_IDirectMusic_5, 5, 0, 0, 0 },
    { "IDirectMusic::EnumMasterClock", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusic::GetMasterClock",
      "IDirectMusic::GetMasterClock: by-value LPGUID is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusic::SetMasterClock", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusic::Activate", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusic::GetDefaultPort",
      "IDirectMusic::GetDefaultPort: by-value LPGUID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusic::SetDirectSound", NULL, cls_IDirectMusic_11, NULL, 3, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectMusicAudioPath[7] =
{
    { "IDirectMusicAudioPath::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicAudioPath::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicAudioPath::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicAudioPath::GetObjectInPath",
      "IDirectMusicAudioPath::GetObjectInPath arg 6 is a void** with no preceding REFIID and is not in RAW_VOID_OUT",
      NULL, NULL, 8, 0, 0, 0 },
    { "IDirectMusicAudioPath::Activate", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicAudioPath::SetVolume", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicAudioPath::ConvertPChannel", NULL, NULL, NULL, 3, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicBand_3[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicBand_3[] = { 23 };
static const unsigned char cls_IDirectMusicBand_4[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicBand_5[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicBand[6] =
{
    { "IDirectMusicBand::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBand::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBand::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBand::CreateSegment", NULL, cls_IDirectMusicBand_3, xaux_IDirectMusicBand_3, 2, 0, 0, 0 },
    { "IDirectMusicBand::Download", NULL, cls_IDirectMusicBand_4, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBand::Unload", NULL, cls_IDirectMusicBand_5, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectMusicBuffer[16] =
{
    { "IDirectMusicBuffer::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBuffer::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBuffer::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicBuffer::Flush", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectMusicBuffer::TotalTime",
      "IDirectMusicBuffer::TotalTime: by-value LPREFERENCE_TIME is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::PackStructured", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicBuffer::PackUnstructured",
      "IDirectMusicBuffer::PackUnstructured: by-value LPBYTE is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IDirectMusicBuffer::ResetReadPtr", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectMusicBuffer::GetNextEvent",
      "IDirectMusicBuffer::GetNextEvent: by-value LPREFERENCE_TIME is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IDirectMusicBuffer::GetRawBufferPtr", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::GetStartTime",
      "IDirectMusicBuffer::GetStartTime: by-value LPREFERENCE_TIME is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::GetUsedBytes",
      "IDirectMusicBuffer::GetUsedBytes: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::GetMaxBytes",
      "IDirectMusicBuffer::GetMaxBytes: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::GetBufferFormat",
      "IDirectMusicBuffer::GetBufferFormat: by-value LPGUID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::SetStartTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicBuffer::SetUsedBytes", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectMusicDownloadedInstrument[3] =
{
    { "IDirectMusicDownloadedInstrument::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicDownloadedInstrument::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicDownloadedInstrument::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
};

static const unsigned char cls_IDirectMusicGetLoader_3[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicGetLoader_3[] = { 17 };
static const struct winecom_slot slots_IDirectMusicGetLoader[4] =
{
    { "IDirectMusicGetLoader::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGetLoader::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGetLoader::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGetLoader::GetLoader", NULL, cls_IDirectMusicGetLoader_3, xaux_IDirectMusicGetLoader_3, 2, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicGraph_4[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicGraph_5[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicGraph_5[] = { 0, 26 };
static const unsigned char cls_IDirectMusicGraph_6[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicGraph[7] =
{
    { "IDirectMusicGraph::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGraph::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGraph::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicGraph::StampPMsg",
      "IDirectMusicGraph::StampPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicGraph::InsertTool", NULL, cls_IDirectMusicGraph_4, NULL, 5, 0, 0, 0 },
    { "IDirectMusicGraph::GetTool", NULL, cls_IDirectMusicGraph_5, xaux_IDirectMusicGraph_5, 3, 0, 0, 0 },
    { "IDirectMusicGraph::RemoveTool", NULL, cls_IDirectMusicGraph_6, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectMusicInstrument[5] =
{
    { "IDirectMusicInstrument::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicInstrument::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicInstrument::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicInstrument::GetPatch", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicInstrument::SetPatch", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicLoader_7[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicLoader_8[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicLoader[12] =
{
    { "IDirectMusicLoader::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader::GetObject",
      "IDirectMusicLoader::GetObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader::SetObject",
      "IDirectMusicLoader::SetObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader::SetSearchDirectory", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader::ScanDirectory", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader::CacheObject", NULL, cls_IDirectMusicLoader_7, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader::ReleaseObject", NULL, cls_IDirectMusicLoader_8, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader::ClearCache", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader::EnableCache", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicLoader::EnumObject",
      "IDirectMusicLoader::EnumObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicLoader8_7[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicLoader8_8[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicLoader8_13[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicLoader8[15] =
{
    { "IDirectMusicLoader8::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader8::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader8::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicLoader8::GetObject",
      "IDirectMusicLoader8::GetObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader8::SetObject",
      "IDirectMusicLoader8::SetObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader8::SetSearchDirectory", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader8::ScanDirectory", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader8::CacheObject", NULL, cls_IDirectMusicLoader8_7, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader8::ReleaseObject", NULL, cls_IDirectMusicLoader8_8, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader8::ClearCache", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader8::EnableCache", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicLoader8::EnumObject",
      "IDirectMusicLoader8::EnumObject: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicLoader8::CollectGarbage", NULL, NULL, NULL, 1, 1, 0, 0 },
    { "IDirectMusicLoader8::ReleaseObjectByUnknown", NULL, cls_IDirectMusicLoader8_13, NULL, 2, 0, 0, 0 },
    { "IDirectMusicLoader8::LoadObjectFromFile",
      "IDirectMusicLoader8::LoadObjectFromFile arg 3 is a void** with no preceding REFIID and is not in RAW_VOID_OUT",
      NULL, NULL, 5, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectMusicObject[6] =
{
    { "IDirectMusicObject::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicObject::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicObject::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicObject::GetDescriptor",
      "IDirectMusicObject::GetDescriptor: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicObject::SetDescriptor",
      "IDirectMusicObject::SetDescriptor: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicObject::ParseDescriptor",
      "IDirectMusicObject::ParseDescriptor: DMUS_OBJECTDESC carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicPerformance_3[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance_3[] = { 9, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance_4[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance_4[] = { 0, 0, 0, 25 };
static const unsigned char cls_IDirectMusicPerformance_5[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance_6[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance_6[] = { 25, 0 };
static const unsigned char cls_IDirectMusicPerformance_14[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance_18[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance_18[] = { 15 };
static const unsigned char cls_IDirectMusicPerformance_19[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance_24[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance_25[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance_26[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance_27[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance_28[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance_28[] = { 0, 22, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance_29[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance_29[] = { 0, 0, 13, 0, 0, 22, 0, 0 };
static const struct winecom_slot slots_IDirectMusicPerformance[44] =
{
    { "IDirectMusicPerformance::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance::Init", NULL, cls_IDirectMusicPerformance_3, xaux_IDirectMusicPerformance_3, 4, 0, 0, 0 },
    { "IDirectMusicPerformance::PlaySegment", NULL, cls_IDirectMusicPerformance_4, xaux_IDirectMusicPerformance_4, 5, 0, 0, 0 },
    { "IDirectMusicPerformance::Stop", NULL, cls_IDirectMusicPerformance_5, NULL, 5, 0, 0, 0 },
    { "IDirectMusicPerformance::GetSegmentState", NULL, cls_IDirectMusicPerformance_6, xaux_IDirectMusicPerformance_6, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::SetPrepareTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::GetPrepareTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::SetBumperLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::GetBumperLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::SendPMsg",
      "IDirectMusicPerformance::SendPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::MusicToReferenceTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::ReferenceToMusicTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::IsPlaying", NULL, cls_IDirectMusicPerformance_14, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::GetTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::AllocPMsg",
      "IDirectMusicPerformance::AllocPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::FreePMsg",
      "IDirectMusicPerformance::FreePMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::GetGraph", NULL, cls_IDirectMusicPerformance_18, xaux_IDirectMusicPerformance_18, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::SetGraph", NULL, cls_IDirectMusicPerformance_19, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::SetNotificationHandle", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::GetNotificationPMsg",
      "IDirectMusicPerformance::GetNotificationPMsg: DMUS_NOTIFICATION_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::AddNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::RemoveNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::AddPort", NULL, cls_IDirectMusicPerformance_24, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::RemovePort", NULL, cls_IDirectMusicPerformance_25, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::AssignPChannelBlock", NULL, cls_IDirectMusicPerformance_26, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance::AssignPChannel", NULL, cls_IDirectMusicPerformance_27, NULL, 5, 0, 0, 0 },
    { "IDirectMusicPerformance::PChannelInfo", NULL, cls_IDirectMusicPerformance_28, xaux_IDirectMusicPerformance_28, 5, 0, 0, 0 },
    { "IDirectMusicPerformance::DownloadInstrument", NULL, cls_IDirectMusicPerformance_29, xaux_IDirectMusicPerformance_29, 9, 0, 0, 0 },
    { "IDirectMusicPerformance::Invalidate", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance::GetParam",
      "IDirectMusicPerformance::GetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicPerformance::SetParam",
      "IDirectMusicPerformance::SetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance::GetGlobalParam", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance::SetGlobalParam", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance::GetLatencyTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::GetQueueTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::AdjustTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance::CloseDown", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectMusicPerformance::GetResolvedTime", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance::MIDIToMusic", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance::MusicToMIDI", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance::TimeToRhythm", NULL, NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicPerformance::RhythmToTime",
      "IDirectMusicPerformance::RhythmToTime: by-value short is not provably integer-class",
      NULL, NULL, 7, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicPerformance8_3[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance8_3[] = { 9, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance8_4[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance8_4[] = { 0, 0, 0, 25 };
static const unsigned char cls_IDirectMusicPerformance8_5[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance8_6[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance8_6[] = { 25, 0 };
static const unsigned char cls_IDirectMusicPerformance8_14[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance8_18[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance8_18[] = { 15 };
static const unsigned char cls_IDirectMusicPerformance8_19[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance8_24[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance8_25[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance8_26[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance8_27[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance8_28[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance8_28[] = { 0, 22, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance8_29[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPerformance8_29[] = { 0, 0, 13, 0, 0, 22, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance8_45[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN };
static const unsigned char xaux_IDirectMusicPerformance8_45[] = { 0, 0, 0, 0, 0, 25, 0, 0 };
static const unsigned char cls_IDirectMusicPerformance8_46[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicPerformance8_48[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance8_48[] = { 0, 0, 10 };
static const unsigned char cls_IDirectMusicPerformance8_50[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPerformance8_51[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPerformance8_51[] = { 10 };
static const struct winecom_slot slots_IDirectMusicPerformance8[53] =
{
    { "IDirectMusicPerformance8::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance8::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance8::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPerformance8::Init", NULL, cls_IDirectMusicPerformance8_3, xaux_IDirectMusicPerformance8_3, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::PlaySegment", NULL, cls_IDirectMusicPerformance8_4, xaux_IDirectMusicPerformance8_4, 5, 0, 0, 0 },
    { "IDirectMusicPerformance8::Stop", NULL, cls_IDirectMusicPerformance8_5, NULL, 5, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetSegmentState", NULL, cls_IDirectMusicPerformance8_6, xaux_IDirectMusicPerformance8_6, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetPrepareTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetPrepareTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetBumperLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetBumperLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::SendPMsg",
      "IDirectMusicPerformance8::SendPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::MusicToReferenceTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::ReferenceToMusicTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::IsPlaying", NULL, cls_IDirectMusicPerformance8_14, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::AllocPMsg",
      "IDirectMusicPerformance8::AllocPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::FreePMsg",
      "IDirectMusicPerformance8::FreePMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetGraph", NULL, cls_IDirectMusicPerformance8_18, xaux_IDirectMusicPerformance8_18, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetGraph", NULL, cls_IDirectMusicPerformance8_19, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetNotificationHandle", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetNotificationPMsg",
      "IDirectMusicPerformance8::GetNotificationPMsg: DMUS_NOTIFICATION_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::AddNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::RemoveNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::AddPort", NULL, cls_IDirectMusicPerformance8_24, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::RemovePort", NULL, cls_IDirectMusicPerformance8_25, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::AssignPChannelBlock", NULL, cls_IDirectMusicPerformance8_26, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::AssignPChannel", NULL, cls_IDirectMusicPerformance8_27, NULL, 5, 0, 0, 0 },
    { "IDirectMusicPerformance8::PChannelInfo", NULL, cls_IDirectMusicPerformance8_28, xaux_IDirectMusicPerformance8_28, 5, 0, 0, 0 },
    { "IDirectMusicPerformance8::DownloadInstrument", NULL, cls_IDirectMusicPerformance8_29, xaux_IDirectMusicPerformance8_29, 9, 0, 0, 0 },
    { "IDirectMusicPerformance8::Invalidate", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetParam",
      "IDirectMusicPerformance8::GetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetParam",
      "IDirectMusicPerformance8::SetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetGlobalParam", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetGlobalParam", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetLatencyTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetQueueTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::AdjustTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::CloseDown", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetResolvedTime", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::MIDIToMusic", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance8::MusicToMIDI", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicPerformance8::TimeToRhythm", NULL, NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicPerformance8::RhythmToTime",
      "IDirectMusicPerformance8::RhythmToTime: by-value short is not provably integer-class",
      NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicPerformance8::InitAudio",
      "IDirectMusicPerformance8::InitAudio arg 0 (IDirectMusic **ppDirectMusic) is array-capable and the method declares a count argument, but ARRAY_SPECS has no entry",
      NULL, NULL, 8, 0, 0, 0 },
    { "IDirectMusicPerformance8::PlaySegmentEx", NULL, cls_IDirectMusicPerformance8_45, xaux_IDirectMusicPerformance8_45, 9, 0, 0, 0 },
    { "IDirectMusicPerformance8::StopEx", NULL, cls_IDirectMusicPerformance8_46, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::ClonePMsg",
      "IDirectMusicPerformance8::ClonePMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPerformance8::CreateAudioPath", NULL, cls_IDirectMusicPerformance8_48, xaux_IDirectMusicPerformance8_48, 4, 0, 0, 0 },
    { "IDirectMusicPerformance8::CreateStandardAudioPath",
      "IDirectMusicPerformance8::CreateStandardAudioPath arg 3 (IDirectMusicAudioPath **ppNewPath) is array-capable and the method declares a count argument, but ARRAY_SPECS has no entry",
      NULL, NULL, 5, 0, 0, 0 },
    { "IDirectMusicPerformance8::SetDefaultAudioPath", NULL, cls_IDirectMusicPerformance8_50, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetDefaultAudioPath", NULL, cls_IDirectMusicPerformance8_51, xaux_IDirectMusicPerformance8_51, 2, 0, 0, 0 },
    { "IDirectMusicPerformance8::GetParamEx",
      "IDirectMusicPerformance8::GetParamEx: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 8, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicPort_3[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPort_5[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPort_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicPort_6[] = { 0, 13, 0, 0 };
static const unsigned char cls_IDirectMusicPort_7[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicPort_8[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicPort_8[] = { 54 };
static const unsigned char cls_IDirectMusicPort_18[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicPort[20] =
{
    { "IDirectMusicPort::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPort::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPort::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicPort::PlayBuffer", NULL, cls_IDirectMusicPort_3, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::SetReadNotificationHandle", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::Read", NULL, cls_IDirectMusicPort_5, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::DownloadInstrument", NULL, cls_IDirectMusicPort_6, xaux_IDirectMusicPort_6, 5, 0, 0, 0 },
    { "IDirectMusicPort::UnloadInstrument", NULL, cls_IDirectMusicPort_7, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::GetLatencyClock", NULL, cls_IDirectMusicPort_8, xaux_IDirectMusicPort_8, 2, 0, 0, 0 },
    { "IDirectMusicPort::GetRunningStats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::Compact", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectMusicPort::GetCaps", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::DeviceIoControl",
      "IDirectMusicPort::DeviceIoControl: by-value LPVOID is not provably integer-class",
      NULL, NULL, 8, 0, 0, 0 },
    { "IDirectMusicPort::SetNumChannelGroups", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::GetNumChannelGroups",
      "IDirectMusicPort::GetNumChannelGroups: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::Activate", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicPort::SetChannelPriority", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPort::GetChannelPriority",
      "IDirectMusicPort::GetChannelPriority: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicPort::SetDirectSound", NULL, cls_IDirectMusicPort_18, NULL, 3, 0, 0, 0 },
    { "IDirectMusicPort::GetFormat",
      "IDirectMusicPort::GetFormat: by-value LPWAVEFORMATEX is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicSegment_9[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment_9[] = { 0, 0, 0, 27 };
static const unsigned char cls_IDirectMusicSegment_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicSegment_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicSegment_12[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicSegment_13[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicSegment_13[] = { 25, 0, 0 };
static const unsigned char cls_IDirectMusicSegment_14[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment_14[] = { 15 };
static const unsigned char cls_IDirectMusicSegment_15[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicSegment_20[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment_20[] = { 0, 0, 23 };
static const struct winecom_slot slots_IDirectMusicSegment[26] =
{
    { "IDirectMusicSegment::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment::GetLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::SetLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::GetRepeats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::SetRepeats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::GetDefaultResolution", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::SetDefaultResolution", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::GetTrack", NULL, cls_IDirectMusicSegment_9, xaux_IDirectMusicSegment_9, 5, 0, 0, 0 },
    { "IDirectMusicSegment::GetTrackGroup", NULL, cls_IDirectMusicSegment_10, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment::InsertTrack", NULL, cls_IDirectMusicSegment_11, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment::RemoveTrack", NULL, cls_IDirectMusicSegment_12, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::InitPlay", NULL, cls_IDirectMusicSegment_13, xaux_IDirectMusicSegment_13, 4, 0, 0, 0 },
    { "IDirectMusicSegment::GetGraph", NULL, cls_IDirectMusicSegment_14, xaux_IDirectMusicSegment_14, 2, 0, 0, 0 },
    { "IDirectMusicSegment::SetGraph", NULL, cls_IDirectMusicSegment_15, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::AddNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::RemoveNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::GetParam",
      "IDirectMusicSegment::GetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicSegment::SetParam",
      "IDirectMusicSegment::SetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicSegment::Clone", NULL, cls_IDirectMusicSegment_20, xaux_IDirectMusicSegment_20, 4, 0, 0, 0 },
    { "IDirectMusicSegment::SetStartPoint", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::GetStartPoint", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment::SetLoopPoints", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment::GetLoopPoints", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment::SetPChannelsUsed", NULL, NULL, NULL, 3, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicSegment8_9[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment8_9[] = { 0, 0, 0, 27 };
static const unsigned char cls_IDirectMusicSegment8_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicSegment8_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicSegment8_12[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicSegment8_13[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IDirectMusicSegment8_13[] = { 25, 0, 0 };
static const unsigned char cls_IDirectMusicSegment8_14[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment8_14[] = { 15 };
static const unsigned char cls_IDirectMusicSegment8_15[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicSegment8_20[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment8_20[] = { 0, 0, 23 };
static const unsigned char cls_IDirectMusicSegment8_27[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment8_27[] = { 63 };
static const unsigned char cls_IDirectMusicSegment8_28[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegment8_28[] = { 0, 0, 0, 23 };
static const unsigned char cls_IDirectMusicSegment8_29[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicSegment8_30[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicSegment8[31] =
{
    { "IDirectMusicSegment8::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment8::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment8::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegment8::GetLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::SetLength", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::GetRepeats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::SetRepeats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::GetDefaultResolution", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::SetDefaultResolution", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::GetTrack", NULL, cls_IDirectMusicSegment8_9, xaux_IDirectMusicSegment8_9, 5, 0, 0, 0 },
    { "IDirectMusicSegment8::GetTrackGroup", NULL, cls_IDirectMusicSegment8_10, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment8::InsertTrack", NULL, cls_IDirectMusicSegment8_11, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment8::RemoveTrack", NULL, cls_IDirectMusicSegment8_12, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::InitPlay", NULL, cls_IDirectMusicSegment8_13, xaux_IDirectMusicSegment8_13, 4, 0, 0, 0 },
    { "IDirectMusicSegment8::GetGraph", NULL, cls_IDirectMusicSegment8_14, xaux_IDirectMusicSegment8_14, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::SetGraph", NULL, cls_IDirectMusicSegment8_15, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::AddNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::RemoveNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::GetParam",
      "IDirectMusicSegment8::GetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 7, 0, 0, 0 },
    { "IDirectMusicSegment8::SetParam",
      "IDirectMusicSegment8::SetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicSegment8::Clone", NULL, cls_IDirectMusicSegment8_20, xaux_IDirectMusicSegment8_20, 4, 0, 0, 0 },
    { "IDirectMusicSegment8::SetStartPoint", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::GetStartPoint", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::SetLoopPoints", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment8::GetLoopPoints", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment8::SetPChannelsUsed", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicSegment8::SetTrackConfig", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicSegment8::GetAudioPathConfig", NULL, cls_IDirectMusicSegment8_27, xaux_IDirectMusicSegment8_27, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::Compose", NULL, cls_IDirectMusicSegment8_28, xaux_IDirectMusicSegment8_28, 5, 0, 0, 0 },
    { "IDirectMusicSegment8::Download", NULL, cls_IDirectMusicSegment8_29, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegment8::Unload", NULL, cls_IDirectMusicSegment8_30, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicSegmentState_4[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicSegmentState_4[] = { 23 };
static const struct winecom_slot slots_IDirectMusicSegmentState[8] =
{
    { "IDirectMusicSegmentState::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegmentState::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegmentState::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicSegmentState::GetRepeats", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegmentState::GetSegment", NULL, cls_IDirectMusicSegmentState_4, xaux_IDirectMusicSegmentState_4, 2, 0, 0, 0 },
    { "IDirectMusicSegmentState::GetStartTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegmentState::GetSeek", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicSegmentState::GetStartPoint", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicTool_3[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IDirectMusicTool[9] =
{
    { "IDirectMusicTool::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTool::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTool::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTool::Init", NULL, cls_IDirectMusicTool_3, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTool::GetMsgDeliveryType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTool::GetMediaTypeArraySize", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTool::GetMediaTypes", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicTool::ProcessPMsg",
      "IDirectMusicTool::ProcessPMsg: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectMusicTool::Flush",
      "IDirectMusicTool::Flush: DMUS_PMSG carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
};

static const unsigned char cls_IDirectMusicTrack_3[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IDirectMusicTrack_6[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IDirectMusicTrack_12[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDirectMusicTrack_12[] = { 0, 0, 27 };
static const struct winecom_slot slots_IDirectMusicTrack[13] =
{
    { "IDirectMusicTrack::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTrack::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTrack::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectMusicTrack::Init", NULL, cls_IDirectMusicTrack_3, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTrack::InitPlay",
      "IDirectMusicTrack::InitPlay arg 2 is a void** with no preceding REFIID and is not in RAW_VOID_OUT",
      NULL, NULL, 6, 0, 0, 0 },
    { "IDirectMusicTrack::EndPlay", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTrack::Play", NULL, cls_IDirectMusicTrack_6, NULL, 9, 0, 0, 0 },
    { "IDirectMusicTrack::GetParam",
      "IDirectMusicTrack::GetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 5, 0, 0, 0 },
    { "IDirectMusicTrack::SetParam",
      "IDirectMusicTrack::SetParam: tag-dispatched void* payload (rguidType chooses the real type; GUID_BandParam's carries an IDirectMusicBand*)",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectMusicTrack::IsParamSupported", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTrack::AddNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTrack::RemoveNotificationType", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectMusicTrack::Clone", NULL, cls_IDirectMusicTrack_12, xaux_IDirectMusicTrack_12, 4, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectSound[11] =
{
    { "IDirectSound::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectSound::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectSound::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectSound::CreateSoundBuffer",
      "IDirectSound::CreateSoundBuffer: by-value LPCDSBUFFERDESC is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectSound::GetCaps", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSound::DuplicateSoundBuffer",
      "IDirectSound::DuplicateSoundBuffer: by-value LPLPDIRECTSOUNDBUFFER is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectSound::SetCooperativeLevel", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IDirectSound::Compact", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectSound::GetSpeakerConfig",
      "IDirectSound::GetSpeakerConfig: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSound::SetSpeakerConfig", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSound::Initialize",
      "IDirectSound::Initialize: by-value LPCGUID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IDirectSoundBuffer[21] =
{
    { "IDirectSoundBuffer::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IDirectSoundBuffer::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectSoundBuffer::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDirectSoundBuffer::GetCaps", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::GetCurrentPosition",
      "IDirectSoundBuffer::GetCurrentPosition: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectSoundBuffer::GetFormat",
      "IDirectSoundBuffer::GetFormat: by-value LPWAVEFORMATEX is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
    { "IDirectSoundBuffer::GetVolume",
      "IDirectSoundBuffer::GetVolume: by-value LPLONG is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::GetPan",
      "IDirectSoundBuffer::GetPan: by-value LPLONG is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::GetFrequency",
      "IDirectSoundBuffer::GetFrequency: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::GetStatus",
      "IDirectSoundBuffer::GetStatus: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::Initialize",
      "IDirectSoundBuffer::Initialize: by-value LPCDSBUFFERDESC is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IDirectSoundBuffer::Lock",
      "IDirectSoundBuffer::Lock: by-value LPDWORD is not provably integer-class",
      NULL, NULL, 8, 0, 0, 0 },
    { "IDirectSoundBuffer::Play", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IDirectSoundBuffer::SetCurrentPosition", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::SetFormat",
      "IDirectSoundBuffer::SetFormat: by-value LPCWAVEFORMATEX is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::SetVolume", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::SetPan", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::SetFrequency", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDirectSoundBuffer::Stop", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IDirectSoundBuffer::Unlock",
      "IDirectSoundBuffer::Unlock: by-value LPVOID is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IDirectSoundBuffer::Restore", NULL, NULL, NULL, 1, 0, 0, 0 },
};

static const unsigned char cls_IDispatch_4[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IDispatch_4[] = { 0, 0, 61 };
static const struct winecom_slot slots_IDispatch[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IDispatch::GetTypeInfoCount", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IDispatch::GetTypeInfo", NULL, cls_IDispatch_4, xaux_IDispatch_4, 4, 0, 0, 0 },
    { "IDispatch::GetIDsOfNames", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "IDispatch::Invoke",
      "IDispatch::Invoke: DISPPARAMS carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 9, 0, 0, 0 },
};

static const unsigned char cls_IEnumConnectionPoints_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumConnectionPoints_6[] = { 31 };
static const struct winecom_slot slots_IEnumConnectionPoints[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumConnectionPoints::Next",
      "IEnumConnectionPoints::Next arg 1 (IConnectionPoint **ppCP) is array-capable and the method declares a count argument, but ARRAY_SPECS has no entry",
      NULL, NULL, 4, 0, 0, 0 },
    { "IEnumConnectionPoints::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumConnectionPoints::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumConnectionPoints::Clone", NULL, cls_IEnumConnectionPoints_6, xaux_IEnumConnectionPoints_6, 2, 0, 0, 0 },
};

static const unsigned char cls_IEnumConnections_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumConnections_6[] = { 32 };
static const struct winecom_slot slots_IEnumConnections[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumConnections::Next",
      "IEnumConnections::Next: CONNECTDATA carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 4, 0, 0, 0 },
    { "IEnumConnections::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumConnections::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumConnections::Clone", NULL, cls_IEnumConnections_6, xaux_IEnumConnections_6, 2, 0, 0, 0 },
};

static const unsigned char cls_IEnumMoniker_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumMoniker_6[] = { 33 };
static const struct winecom_slot slots_IEnumMoniker[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumMoniker::Next",
      "IEnumMoniker::Next arg 1 (IMoniker **rgelt) is array-capable and the method declares a count argument, but ARRAY_SPECS has no entry",
      NULL, NULL, 4, 0, 0, 0 },
    { "IEnumMoniker::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumMoniker::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumMoniker::Clone", NULL, cls_IEnumMoniker_6, xaux_IEnumMoniker_6, 2, 0, 0, 0 },
};

static const unsigned char cls_IEnumSTATSTG_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumSTATSTG_6[] = { 34 };
static const struct winecom_slot slots_IEnumSTATSTG[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumSTATSTG::Next", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IEnumSTATSTG::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumSTATSTG::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumSTATSTG::Clone", NULL, cls_IEnumSTATSTG_6, xaux_IEnumSTATSTG_6, 2, 0, 0, 0 },
};

static const unsigned char cls_IEnumString_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumString_6[] = { 35 };
static const struct winecom_slot slots_IEnumString[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumString::Next", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IEnumString::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumString::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumString::Clone", NULL, cls_IEnumString_6, xaux_IEnumString_6, 2, 0, 0, 0 },
};

static const unsigned char cls_IEnumUnknown_6[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IEnumUnknown_6[] = { 36 };
static const struct winecom_slot slots_IEnumUnknown[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IEnumUnknown::Next",
      "IEnumUnknown::Next arg 1 (IUnknown **rgelt) is array-capable and the method declares a count argument, but ARRAY_SPECS has no entry",
      NULL, NULL, 4, 0, 0, 0 },
    { "IEnumUnknown::Skip", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IEnumUnknown::Reset", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IEnumUnknown::Clone", NULL, cls_IEnumUnknown_6, xaux_IEnumUnknown_6, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IErrorInfo[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IErrorInfo::GetGUID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IErrorInfo::GetSource", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IErrorInfo::GetDescription", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IErrorInfo::GetHelpFile", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IErrorInfo::GetHelpContext", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IGlobalInterfaceTable_3[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IGlobalInterfaceTable_5[] = { WINECOM_CA_PASS, WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const struct winecom_slot slots_IGlobalInterfaceTable[6] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IGlobalInterfaceTable::RegisterInterfaceInGlobal", NULL, cls_IGlobalInterfaceTable_3, NULL, 4, 0, 0, 0 },
    { "IGlobalInterfaceTable::RevokeInterfaceFromGlobal", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IGlobalInterfaceTable::GetInterfaceFromGlobal", NULL, cls_IGlobalInterfaceTable_5, NULL, 4, 0, 1, 0 },
};

static const struct winecom_slot slots_IInspectable[6] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IInspectable::GetIids", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IInspectable::GetRuntimeClassName", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IInspectable::GetTrustLevel", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_ILockBytes[10] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ILockBytes::ReadAt", NULL, NULL, NULL, 5, 0, 0, 0 },
    { "ILockBytes::WriteAt", NULL, NULL, NULL, 5, 0, 0, 0 },
    { "ILockBytes::Flush", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "ILockBytes::SetSize", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "ILockBytes::LockRegion", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ILockBytes::UnlockRegion", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ILockBytes::Stat", NULL, NULL, NULL, 3, 0, 0, 0 },
};

static const struct winecom_slot slots_IMMDevice[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMMDevice::Activate", NULL, NULL, NULL, 5, WINECOM_F_HAND, 5, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMDevice::OpenPropertyStore",
      "IMMDevice::OpenPropertyStore: takes `IPropertyStore **ppProperties`, an interface pointer of a type the wine-syscom roster does not carry -- there is no guest stub vtable for it, so it can be neither wrapped on the way out nor recognised on the way in",
      NULL, NULL, 3, 0, 0, 0 },
    { "IMMDevice::GetId", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMDevice::GetState", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IMMDeviceCollection_4[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMMDeviceCollection_4[] = { 0, 41 };
static const struct winecom_slot slots_IMMDeviceCollection[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMMDeviceCollection::GetCount", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMDeviceCollection::Item", NULL, cls_IMMDeviceCollection_4, xaux_IMMDeviceCollection_4, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x02 },
};

static const unsigned char cls_IMMDeviceEnumerator_3[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMMDeviceEnumerator_3[] = { 0, 0, 42 };
static const unsigned char cls_IMMDeviceEnumerator_4[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMMDeviceEnumerator_4[] = { 0, 0, 41 };
static const unsigned char cls_IMMDeviceEnumerator_5[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMMDeviceEnumerator_5[] = { 0, 41 };
static const struct winecom_slot slots_IMMDeviceEnumerator[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMMDeviceEnumerator::EnumAudioEndpoints", NULL, cls_IMMDeviceEnumerator_3, xaux_IMMDeviceEnumerator_3, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x04 },
    { "IMMDeviceEnumerator::GetDefaultAudioEndpoint", NULL, cls_IMMDeviceEnumerator_4, xaux_IMMDeviceEnumerator_4, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x04 },
    { "IMMDeviceEnumerator::GetDevice", NULL, cls_IMMDeviceEnumerator_5, xaux_IMMDeviceEnumerator_5, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x02 },
    { "IMMDeviceEnumerator::RegisterEndpointNotificationCallback", NULL, NULL, NULL, 2, WINECOM_F_HAND, 6, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMDeviceEnumerator::UnregisterEndpointNotificationCallback", NULL, NULL, NULL, 2, WINECOM_F_HAND, 7, 0, NULL, 0x00, 0x00, 0x00 },
};

static const struct winecom_slot slots_IMMNotificationClient[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMMNotificationClient::OnDeviceStateChanged", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMNotificationClient::OnDeviceAdded", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMNotificationClient::OnDeviceRemoved", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMNotificationClient::OnDefaultDeviceChanged", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IMMNotificationClient::OnPropertyValueChanged",
      "IMMNotificationClient::OnPropertyValueChanged: by-value parameter `const PROPERTYKEY key` is of a type this generator cannot prove is integer-class on both ABIs; refusing rather than assuming it is an enum",
      NULL, NULL, 3, 0, 0, 0 },
};

static const struct winecom_slot slots_IMalloc[9] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMalloc::Alloc",
      "IMalloc::Alloc: return type LPVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IMalloc::Realloc",
      "IMalloc::Realloc: by-value LPVOID is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IMalloc::Free",
      "IMalloc::Free: by-value LPVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IMalloc::GetSize",
      "IMalloc::GetSize: by-value LPVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IMalloc::DidAlloc",
      "IMalloc::DidAlloc: by-value LPVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IMalloc::HeapMinimize", NULL, NULL, NULL, 1, 1, 0, 0 },
};

static const unsigned char cls_IMarshal_5[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IMarshal_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const unsigned char cls_IMarshal_7[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IMarshal[9] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMarshal::GetUnmarshalClass", NULL, NULL, NULL, 7, 0, 0, 0 },
    { "IMarshal::GetMarshalSizeMax", NULL, NULL, NULL, 7, 0, 0, 0 },
    { "IMarshal::MarshalInterface", NULL, cls_IMarshal_5, NULL, 7, 0, 0, 0 },
    { "IMarshal::UnmarshalInterface", NULL, cls_IMarshal_6, NULL, 4, 0, 1, 0 },
    { "IMarshal::ReleaseMarshalData", NULL, cls_IMarshal_7, NULL, 2, 0, 0, 0 },
    { "IMarshal::DisconnectObject", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IMoniker_5[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IMoniker_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IMoniker_8[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const unsigned char cls_IMoniker_9[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_RIID, WINECOM_CA_PPV_OUT };
static const unsigned char cls_IMoniker_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_10[] = { 0, 0, 47, 47 };
static const unsigned char cls_IMoniker_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_11[] = { 0, 0, 47 };
static const unsigned char cls_IMoniker_12[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_12[] = { 0, 33 };
static const unsigned char cls_IMoniker_13[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IMoniker_15[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN };
static const unsigned char cls_IMoniker_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IMoniker_17[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_17[] = { 47 };
static const unsigned char cls_IMoniker_18[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_18[] = { 0, 47 };
static const unsigned char cls_IMoniker_19[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IMoniker_19[] = { 0, 47 };
static const unsigned char cls_IMoniker_20[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const struct winecom_slot slots_IMoniker[23] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IPersist::GetClassID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IPersistStream::IsDirty", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IPersistStream::Load", NULL, cls_IMoniker_5, NULL, 2, 0, 0, 0 },
    { "IPersistStream::Save", NULL, cls_IMoniker_6, NULL, 3, 0, 0, 0 },
    { "IPersistStream::GetSizeMax", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IMoniker::BindToObject", NULL, cls_IMoniker_8, NULL, 5, 0, 2, 0 },
    { "IMoniker::BindToStorage", NULL, cls_IMoniker_9, NULL, 5, 0, 2, 0 },
    { "IMoniker::Reduce", NULL, cls_IMoniker_10, xaux_IMoniker_10, 5, 0, 0, 0 },
    { "IMoniker::ComposeWith", NULL, cls_IMoniker_11, xaux_IMoniker_11, 4, 0, 0, 0 },
    { "IMoniker::Enum", NULL, cls_IMoniker_12, xaux_IMoniker_12, 3, 0, 0, 0 },
    { "IMoniker::IsEqual", NULL, cls_IMoniker_13, NULL, 2, 0, 0, 0 },
    { "IMoniker::Hash", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IMoniker::IsRunning", NULL, cls_IMoniker_15, NULL, 4, 0, 0, 0 },
    { "IMoniker::GetTimeOfLastChange", NULL, cls_IMoniker_16, NULL, 4, 0, 0, 0 },
    { "IMoniker::Inverse", NULL, cls_IMoniker_17, xaux_IMoniker_17, 2, 0, 0, 0 },
    { "IMoniker::CommonPrefixWith", NULL, cls_IMoniker_18, xaux_IMoniker_18, 3, 0, 0, 0 },
    { "IMoniker::RelativePathTo", NULL, cls_IMoniker_19, xaux_IMoniker_19, 3, 0, 0, 0 },
    { "IMoniker::GetDisplayName", NULL, cls_IMoniker_20, NULL, 4, 0, 0, 0 },
    { "IMoniker::ParseDisplayName",
      "IMoniker::ParseDisplayName: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 6, 0, 0, 0 },
    { "IMoniker::IsSystemMoniker", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IMultiQI[4] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IMultiQI::QueryMultipleInterfaces",
      "IMultiQI::QueryMultipleInterfaces: MULTI_QI carries interface pointers inside a struct and has no hand-written walker",
      NULL, NULL, 3, 0, 0, 0 },
};

static const struct winecom_slot slots_IPersist[4] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IPersist::GetClassID", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IPersistFile[9] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IPersist::GetClassID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IPersistFile::IsDirty", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IPersistFile::Load",
      "IPersistFile::Load: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IPersistFile::Save",
      "IPersistFile::Save: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IPersistFile::SaveCompleted",
      "IPersistFile::SaveCompleted: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IPersistFile::GetCurFile", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IPersistStream_5[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IPersistStream_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const struct winecom_slot slots_IPersistStream[8] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IPersist::GetClassID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IPersistStream::IsDirty", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IPersistStream::Load", NULL, cls_IPersistStream_5, NULL, 2, 0, 0, 0 },
    { "IPersistStream::Save", NULL, cls_IPersistStream_6, NULL, 3, 0, 0, 0 },
    { "IPersistStream::GetSizeMax", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IPersistStreamInit_5[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IPersistStreamInit_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const struct winecom_slot slots_IPersistStreamInit[9] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IPersist::GetClassID", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IPersistStreamInit::IsDirty", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IPersistStreamInit::Load", NULL, cls_IPersistStreamInit_5, NULL, 2, 0, 0, 0 },
    { "IPersistStreamInit::Save", NULL, cls_IPersistStreamInit_6, NULL, 3, 0, 0, 0 },
    { "IPersistStreamInit::GetSizeMax", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IPersistStreamInit::InitNew", NULL, NULL, NULL, 1, 0, 0, 0 },
};

static const unsigned char cls_IRecordInfo_9[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IRecordInfo_9[] = { 61 };
static const unsigned char cls_IRecordInfo_15[] = { WINECOM_CA_IFACE_IN };
static const struct winecom_slot slots_IRecordInfo[19] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IRecordInfo::RecordInit",
      "IRecordInfo::RecordInit: by-value PVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::RecordClear",
      "IRecordInfo::RecordClear: by-value PVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::RecordCopy",
      "IRecordInfo::RecordCopy: by-value PVOID is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IRecordInfo::GetGuid", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::GetName", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::GetSize", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::GetTypeInfo", NULL, cls_IRecordInfo_9, xaux_IRecordInfo_9, 2, 0, 0, 0 },
    { "IRecordInfo::GetField",
      "IRecordInfo::GetField: by-value PVOID is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
    { "IRecordInfo::GetFieldNoCopy",
      "IRecordInfo::GetFieldNoCopy: by-value PVOID is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IRecordInfo::PutField",
      "IRecordInfo::PutField: by-value PVOID is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IRecordInfo::PutFieldNoCopy",
      "IRecordInfo::PutFieldNoCopy: by-value PVOID is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IRecordInfo::GetFieldNames", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IRecordInfo::IsMatchingType", NULL, cls_IRecordInfo_15, NULL, 2, 0, 0, 0 },
    { "IRecordInfo::RecordCreate",
      "IRecordInfo::RecordCreate: return type PVOID is not provably integer-class",
      NULL, NULL, 1, 0, 0, 0 },
    { "IRecordInfo::RecordCreateCopy",
      "IRecordInfo::RecordCreateCopy: by-value PVOID is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IRecordInfo::RecordDestroy",
      "IRecordInfo::RecordDestroy: by-value PVOID is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_IReferenceClock[7] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IReferenceClock::GetTime", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IReferenceClock::AdviseTime",
      "IReferenceClock::AdviseTime: by-value HEVENT is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IReferenceClock::AdvisePeriodic",
      "IReferenceClock::AdvisePeriodic: by-value HSEMAPHORE is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IReferenceClock::Unadvise", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const unsigned char cls_IRunningObjectTable_3[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IRunningObjectTable_5[] = { WINECOM_CA_IFACE_IN };
static const unsigned char cls_IRunningObjectTable_6[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IRunningObjectTable_6[] = { 0, 63 };
static const unsigned char cls_IRunningObjectTable_8[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char cls_IRunningObjectTable_9[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IRunningObjectTable_9[] = { 33 };
static const struct winecom_slot slots_IRunningObjectTable[10] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IRunningObjectTable::Register", NULL, cls_IRunningObjectTable_3, NULL, 5, 0, 0, 0 },
    { "IRunningObjectTable::Revoke", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IRunningObjectTable::IsRunning", NULL, cls_IRunningObjectTable_5, NULL, 2, 0, 0, 0 },
    { "IRunningObjectTable::GetObject", NULL, cls_IRunningObjectTable_6, xaux_IRunningObjectTable_6, 3, 0, 0, 0 },
    { "IRunningObjectTable::NoteChangeTime", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IRunningObjectTable::GetTimeOfLastChange", NULL, cls_IRunningObjectTable_8, NULL, 3, 0, 0, 0 },
    { "IRunningObjectTable::EnumRunning", NULL, cls_IRunningObjectTable_9, xaux_IRunningObjectTable_9, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_ISequentialStream[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ISequentialStream::Read", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ISequentialStream::Write", NULL, NULL, NULL, 4, 0, 0, 0 },
};

static const unsigned char cls_IStorage_11[] = { WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IStorage_11[] = { 0, 0, 0, 34 };
static const struct winecom_slot slots_IStorage[18] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IStorage::CreateStream",
      "IStorage::CreateStream: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 6, 0, 0, 0 },
    { "IStorage::OpenStream",
      "IStorage::OpenStream: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 6, 0, 0, 0 },
    { "IStorage::CreateStorage",
      "IStorage::CreateStorage: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 6, 0, 0, 0 },
    { "IStorage::OpenStorage",
      "IStorage::OpenStorage: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 7, 0, 0, 0 },
    { "IStorage::CopyTo",
      "IStorage::CopyTo: by-value SNB is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IStorage::MoveElementTo",
      "IStorage::MoveElementTo: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IStorage::Commit", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IStorage::Revert", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IStorage::EnumElements", NULL, cls_IStorage_11, xaux_IStorage_11, 5, 0, 0, 0 },
    { "IStorage::DestroyElement",
      "IStorage::DestroyElement: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 2, 0, 0, 0 },
    { "IStorage::RenameElement",
      "IStorage::RenameElement: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 3, 0, 0, 0 },
    { "IStorage::SetElementTimes",
      "IStorage::SetElementTimes: by-value LPCOLESTR is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
    { "IStorage::SetClass", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IStorage::SetStateBits", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IStorage::Stat", NULL, NULL, NULL, 3, 0, 0, 0 },
};

static const unsigned char cls_IStream_7[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_IStream_13[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_IStream_13[] = { 58 };
static const struct winecom_slot slots_IStream[14] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ISequentialStream::Read", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ISequentialStream::Write", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IStream::Seek", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IStream::SetSize", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IStream::CopyTo", NULL, cls_IStream_7, NULL, 5, 0, 0, 0 },
    { "IStream::Commit", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "IStream::Revert", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "IStream::LockRegion", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IStream::UnlockRegion", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "IStream::Stat", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "IStream::Clone", NULL, cls_IStream_13, xaux_IStream_13, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_ISupportErrorInfo[4] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ISupportErrorInfo::InterfaceSupportsErrorInfo", NULL, NULL, NULL, 2, 0, 0, 0 },
};

static const struct winecom_slot slots_ITypeComp[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ITypeComp::Bind",
      "ITypeComp::Bind: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 7, 0, 0, 0 },
    { "ITypeComp::BindType",
      "ITypeComp::BindType: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 5, 0, 0, 0 },
};

static const unsigned char cls_ITypeInfo_4[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_ITypeInfo_4[] = { 60 };
static const unsigned char cls_ITypeInfo_14[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_ITypeInfo_14[] = { 0, 61 };
static const unsigned char cls_ITypeInfo_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char cls_ITypeInfo_18[] = { WINECOM_CA_IFACE_OUT_STATIC, WINECOM_CA_PASS };
static const unsigned char xaux_ITypeInfo_18[] = { 62, 0 };
static const struct winecom_slot slots_ITypeInfo[22] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ITypeInfo::GetTypeAttr", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "ITypeInfo::GetTypeComp", NULL, cls_ITypeInfo_4, xaux_ITypeInfo_4, 2, 0, 0, 0 },
    { "ITypeInfo::GetFuncDesc", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeInfo::GetVarDesc", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeInfo::GetNames", NULL, NULL, NULL, 5, 0, 0, 0 },
    { "ITypeInfo::GetRefTypeOfImplType", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeInfo::GetImplTypeFlags", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeInfo::GetIDsOfNames", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ITypeInfo::Invoke",
      "ITypeInfo::Invoke: by-value PVOID is not provably integer-class",
      NULL, NULL, 8, 0, 0, 0 },
    { "ITypeInfo::GetDocumentation", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "ITypeInfo::GetDllEntry", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "ITypeInfo::GetRefTypeInfo", NULL, cls_ITypeInfo_14, xaux_ITypeInfo_14, 3, 0, 0, 0 },
    { "ITypeInfo::AddressOfMember", NULL, NULL, NULL, 4, 0, 0, 0 },
    { "ITypeInfo::CreateInstance", NULL, cls_ITypeInfo_16, NULL, 4, 0, 0, 0 },
    { "ITypeInfo::GetMops", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeInfo::GetContainingTypeLib", NULL, cls_ITypeInfo_18, xaux_ITypeInfo_18, 3, 0, 0, 0 },
    { "ITypeInfo::ReleaseTypeAttr", NULL, NULL, NULL, 2, 1, 0, 0 },
    { "ITypeInfo::ReleaseFuncDesc", NULL, NULL, NULL, 2, 1, 0, 0 },
    { "ITypeInfo::ReleaseVarDesc", NULL, NULL, NULL, 2, 1, 0, 0 },
};

static const unsigned char cls_ITypeLib_4[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_ITypeLib_4[] = { 0, 61 };
static const unsigned char cls_ITypeLib_6[] = { WINECOM_CA_PASS, WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_ITypeLib_6[] = { 0, 61 };
static const unsigned char cls_ITypeLib_8[] = { WINECOM_CA_IFACE_OUT_STATIC };
static const unsigned char xaux_ITypeLib_8[] = { 60 };
static const struct winecom_slot slots_ITypeLib[13] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "ITypeLib::GetTypeInfoCount", NULL, NULL, NULL, 1, 0, 0, 0 },
    { "ITypeLib::GetTypeInfo", NULL, cls_ITypeLib_4, xaux_ITypeLib_4, 3, 0, 0, 0 },
    { "ITypeLib::GetTypeInfoType", NULL, NULL, NULL, 3, 0, 0, 0 },
    { "ITypeLib::GetTypeInfoOfGuid", NULL, cls_ITypeLib_6, xaux_ITypeLib_6, 3, 0, 0, 0 },
    { "ITypeLib::GetLibAttr", NULL, NULL, NULL, 2, 0, 0, 0 },
    { "ITypeLib::GetTypeComp", NULL, cls_ITypeLib_8, xaux_ITypeLib_8, 2, 0, 0, 0 },
    { "ITypeLib::GetDocumentation", NULL, NULL, NULL, 6, 0, 0, 0 },
    { "ITypeLib::IsName",
      "ITypeLib::IsName: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 4, 0, 0, 0 },
    { "ITypeLib::FindName",
      "ITypeLib::FindName: by-value LPOLESTR is not provably integer-class",
      NULL, NULL, 6, 0, 0, 0 },
    { "ITypeLib::ReleaseTLibAttr", NULL, NULL, NULL, 2, 1, 0, 0 },
};

static const struct winecom_slot slots_IUnknown[3] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
};

static const unsigned char cls_IXAudio2_6[] = { WINECOM_CA_IFACE_IN };
static const unsigned char xaux_IXAudio2_6[] = { 65 };
static const unsigned char cls_IXAudio2_7[] = { WINECOM_CA_IFACE_IN };
static const unsigned char xaux_IXAudio2_7[] = { 65 };
static const struct winecom_slot slots_IXAudio2[16] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0 },  /* runtime */
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0 },  /* runtime */
    { "IXAudio2::GetDeviceCount", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::GetDeviceDetails", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::Initialize", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::RegisterForCallbacks", NULL, cls_IXAudio2_6, xaux_IXAudio2_6, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x01 },
    { "IXAudio2::UnregisterForCallbacks", NULL, cls_IXAudio2_7, xaux_IXAudio2_7, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x01 },
    { "IXAudio2::CreateSourceVoice", NULL, NULL, NULL, 8, WINECOM_F_HAND, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::CreateSubmixVoice", NULL, NULL, NULL, 8, WINECOM_F_HAND, 1, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::CreateMasteringVoice", NULL, NULL, NULL, 7, WINECOM_F_HAND, 2, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::StartEngine", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::StopEngine", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::CommitChanges", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::GetPerformanceData", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2::SetDebugConfiguration", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const struct winecom_slot slots_IXAudio2EngineCallback[3] =
{
    { "IXAudio2EngineCallback::OnProcessingPassStart", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2EngineCallback::OnProcessingPassEnd", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2EngineCallback::OnCriticalError", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IXAudio2MasteringVoice_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2MasteringVoice_10[] = { 69, 0, 0 };
static const unsigned char cls_IXAudio2MasteringVoice_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2MasteringVoice_11[] = { 69, 0 };
static const unsigned char cls_IXAudio2MasteringVoice_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2MasteringVoice_16[] = { 69, 0, 0, 0, 0 };
static const unsigned char cls_IXAudio2MasteringVoice_17[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2MasteringVoice_17[] = { 69, 0, 0, 0 };
static const struct winecom_slot slots_IXAudio2MasteringVoice[19] =
{
    { "IXAudio2Voice::GetVoiceDetails", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputVoices", NULL, NULL, NULL, 2, WINECOM_F_HAND, 3, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectChain", NULL, NULL, NULL, 2, WINECOM_F_HAND, 4, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::EnableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DisableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectState", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectParameters", NULL, NULL, NULL, 5, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectParameters", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetFilterParameters", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetFilterParameters", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputFilterParameters", NULL, cls_IXAudio2MasteringVoice_10, xaux_IXAudio2MasteringVoice_10, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputFilterParameters", NULL, cls_IXAudio2MasteringVoice_11, xaux_IXAudio2MasteringVoice_11, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetVolume", NULL, NULL, NULL, 3, WINECOM_F_HAND|WINECOM_F_REV, 8, 0, NULL, 0x01, 0x00, 0x00 },
    { "IXAudio2Voice::GetVolume", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetChannelVolumes", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetChannelVolumes", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputMatrix", NULL, cls_IXAudio2MasteringVoice_16, xaux_IXAudio2MasteringVoice_16, 6, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputMatrix", NULL, cls_IXAudio2MasteringVoice_17, xaux_IXAudio2MasteringVoice_17, 5, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DestroyVoice", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IXAudio2SourceVoice_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SourceVoice_10[] = { 69, 0, 0 };
static const unsigned char cls_IXAudio2SourceVoice_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SourceVoice_11[] = { 69, 0 };
static const unsigned char cls_IXAudio2SourceVoice_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SourceVoice_16[] = { 69, 0, 0, 0, 0 };
static const unsigned char cls_IXAudio2SourceVoice_17[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SourceVoice_17[] = { 69, 0, 0, 0 };
static const struct winecom_slot slots_IXAudio2SourceVoice[29] =
{
    { "IXAudio2Voice::GetVoiceDetails", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputVoices", NULL, NULL, NULL, 2, WINECOM_F_HAND, 3, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectChain", NULL, NULL, NULL, 2, WINECOM_F_HAND, 4, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::EnableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DisableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectState", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectParameters", NULL, NULL, NULL, 5, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectParameters", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetFilterParameters", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetFilterParameters", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputFilterParameters", NULL, cls_IXAudio2SourceVoice_10, xaux_IXAudio2SourceVoice_10, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputFilterParameters", NULL, cls_IXAudio2SourceVoice_11, xaux_IXAudio2SourceVoice_11, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetVolume", NULL, NULL, NULL, 3, WINECOM_F_HAND|WINECOM_F_REV, 8, 0, NULL, 0x01, 0x00, 0x00 },
    { "IXAudio2Voice::GetVolume", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetChannelVolumes", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetChannelVolumes", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputMatrix", NULL, cls_IXAudio2SourceVoice_16, xaux_IXAudio2SourceVoice_16, 6, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputMatrix", NULL, cls_IXAudio2SourceVoice_17, xaux_IXAudio2SourceVoice_17, 5, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DestroyVoice", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::Start", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::Stop", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::SubmitSourceBuffer", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::FlushSourceBuffers", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::Discontinuity", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::ExitLoop", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::GetState", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::SetFrequencyRatio", NULL, NULL, NULL, 3, WINECOM_F_HAND|WINECOM_F_REV, 8, 0, NULL, 0x01, 0x00, 0x00 },
    { "IXAudio2SourceVoice::GetFrequencyRatio", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2SourceVoice::SetSourceSampleRate", NULL, NULL, NULL, 2, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IXAudio2SubmixVoice_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SubmixVoice_10[] = { 69, 0, 0 };
static const unsigned char cls_IXAudio2SubmixVoice_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SubmixVoice_11[] = { 69, 0 };
static const unsigned char cls_IXAudio2SubmixVoice_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SubmixVoice_16[] = { 69, 0, 0, 0, 0 };
static const unsigned char cls_IXAudio2SubmixVoice_17[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2SubmixVoice_17[] = { 69, 0, 0, 0 };
static const struct winecom_slot slots_IXAudio2SubmixVoice[19] =
{
    { "IXAudio2Voice::GetVoiceDetails", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputVoices", NULL, NULL, NULL, 2, WINECOM_F_HAND, 3, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectChain", NULL, NULL, NULL, 2, WINECOM_F_HAND, 4, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::EnableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DisableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectState", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectParameters", NULL, NULL, NULL, 5, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectParameters", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetFilterParameters", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetFilterParameters", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputFilterParameters", NULL, cls_IXAudio2SubmixVoice_10, xaux_IXAudio2SubmixVoice_10, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputFilterParameters", NULL, cls_IXAudio2SubmixVoice_11, xaux_IXAudio2SubmixVoice_11, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetVolume", NULL, NULL, NULL, 3, WINECOM_F_HAND|WINECOM_F_REV, 8, 0, NULL, 0x01, 0x00, 0x00 },
    { "IXAudio2Voice::GetVolume", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetChannelVolumes", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetChannelVolumes", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputMatrix", NULL, cls_IXAudio2SubmixVoice_16, xaux_IXAudio2SubmixVoice_16, 6, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputMatrix", NULL, cls_IXAudio2SubmixVoice_17, xaux_IXAudio2SubmixVoice_17, 5, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DestroyVoice", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const unsigned char cls_IXAudio2Voice_10[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2Voice_10[] = { 69, 0, 0 };
static const unsigned char cls_IXAudio2Voice_11[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2Voice_11[] = { 69, 0 };
static const unsigned char cls_IXAudio2Voice_16[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2Voice_16[] = { 69, 0, 0, 0, 0 };
static const unsigned char cls_IXAudio2Voice_17[] = { WINECOM_CA_IFACE_IN, WINECOM_CA_PASS, WINECOM_CA_PASS, WINECOM_CA_PASS };
static const unsigned char xaux_IXAudio2Voice_17[] = { 69, 0, 0, 0 };
static const struct winecom_slot slots_IXAudio2Voice[19] =
{
    { "IXAudio2Voice::GetVoiceDetails", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputVoices", NULL, NULL, NULL, 2, WINECOM_F_HAND, 3, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectChain", NULL, NULL, NULL, 2, WINECOM_F_HAND, 4, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::EnableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DisableEffect", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectState", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetEffectParameters", NULL, NULL, NULL, 5, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetEffectParameters", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetFilterParameters", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetFilterParameters", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputFilterParameters", NULL, cls_IXAudio2Voice_10, xaux_IXAudio2Voice_10, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputFilterParameters", NULL, cls_IXAudio2Voice_11, xaux_IXAudio2Voice_11, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetVolume", NULL, NULL, NULL, 3, WINECOM_F_HAND|WINECOM_F_REV, 8, 0, NULL, 0x01, 0x00, 0x00 },
    { "IXAudio2Voice::GetVolume", NULL, NULL, NULL, 2, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetChannelVolumes", NULL, NULL, NULL, 4, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetChannelVolumes", NULL, NULL, NULL, 3, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::SetOutputMatrix", NULL, cls_IXAudio2Voice_16, xaux_IXAudio2Voice_16, 6, 0, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::GetOutputMatrix", NULL, cls_IXAudio2Voice_17, xaux_IXAudio2Voice_17, 5, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
    { "IXAudio2Voice::DestroyVoice", NULL, NULL, NULL, 1, WINECOM_F_RET_VOID, 0, 0, NULL, 0x00, 0x00, 0x00 },
};

static const struct winecom_iface syscom_com_ifaces[SYSCOM_IFACE_COUNT] =
{
    { "IActivationFactory", {0x00000035,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IActivationFactory, 0 },
    { "IAgileObject", {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}},
      3, slots_IAgileObject, 0 },
    { "IAudioClient", {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}},
      15, slots_IAudioClient, 0 },
    { "IAudioRenderClient", {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}},
      5, slots_IAudioRenderClient, 0 },
    { "IBindCtx", {0x0000000e,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      13, slots_IBindCtx, 0 },
    { "IClassFactory", {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      5, slots_IClassFactory, 0 },
    { "IConnectionPoint", {0xb196b286,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07}},
      8, slots_IConnectionPoint, 0 },
    { "IConnectionPointContainer", {0xb196b284,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07}},
      5, slots_IConnectionPointContainer, 0 },
    { "ICreateErrorInfo", {0x22f03340,0x547d,0x101b,{0x8e,0x65,0x08,0x00,0x2b,0x2b,0xd1,0x19}},
      8, slots_ICreateErrorInfo, 0 },
    { "IDirectMusic", {0x6536115a,0x7b2d,0x11d2,{0xba,0x18,0x00,0x00,0xf8,0x75,0xac,0x12}},
      12, slots_IDirectMusic, 0 },
    { "IDirectMusicAudioPath", {0xc87631f5,0x23be,0x4986,{0x88,0x36,0x05,0x83,0x2f,0xcc,0x48,0xf9}},
      7, slots_IDirectMusicAudioPath, 0 },
    { "IDirectMusicBand", {0xd2ac28c0,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      6, slots_IDirectMusicBand, 0 },
    { "IDirectMusicBuffer", {0xd2ac2878,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      16, slots_IDirectMusicBuffer, 0 },
    { "IDirectMusicDownloadedInstrument", {0xd2ac287e,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      3, slots_IDirectMusicDownloadedInstrument, 0 },
    { "IDirectMusicGetLoader", {0x68a04844,0xd13d,0x11d1,{0xaf,0xa6,0x00,0xaa,0x00,0x24,0xd8,0xb6}},
      4, slots_IDirectMusicGetLoader, 0 },
    { "IDirectMusicGraph", {0x2befc277,0x5497,0x11d2,{0xbc,0xcb,0x00,0xa0,0xc9,0x22,0xe6,0xeb}},
      7, slots_IDirectMusicGraph, 0 },
    { "IDirectMusicInstrument", {0xd2ac287d,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      5, slots_IDirectMusicInstrument, 0 },
    { "IDirectMusicLoader", {0x2ffaaca2,0x5dca,0x11d2,{0xaf,0xa6,0x00,0xaa,0x00,0x24,0xd8,0xb6}},
      12, slots_IDirectMusicLoader, 0 },
    { "IDirectMusicLoader8", {0x19e7c08c,0x0a44,0x4e6a,{0xa1,0x16,0x59,0x5a,0x7c,0xd5,0xde,0x8c}},
      15, slots_IDirectMusicLoader8, 0 },
    { "IDirectMusicObject", {0xd2ac28b5,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      6, slots_IDirectMusicObject, 0 },
    { "IDirectMusicPerformance", {0x07d43d03,0x6523,0x11d2,{0x87,0x1d,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      44, slots_IDirectMusicPerformance, 0 },
    { "IDirectMusicPerformance8", {0x679c4137,0xc62e,0x4147,{0xb2,0xb4,0x9d,0x56,0x9a,0xcb,0x25,0x4c}},
      53, slots_IDirectMusicPerformance8, 0 },
    { "IDirectMusicPort", {0x08f2d8c9,0x37c2,0x11d2,{0xb9,0xf9,0x00,0x00,0xf8,0x75,0xac,0x12}},
      20, slots_IDirectMusicPort, 0 },
    { "IDirectMusicSegment", {0xf96029a2,0x4282,0x11d2,{0x87,0x17,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      26, slots_IDirectMusicSegment, 0 },
    { "IDirectMusicSegment8", {0xc6784488,0x41a3,0x418f,{0xaa,0x15,0xb3,0x50,0x93,0xba,0x42,0xd4}},
      31, slots_IDirectMusicSegment8, 0 },
    { "IDirectMusicSegmentState", {0xa3afdcc7,0xd3ee,0x11d1,{0xbc,0x8d,0x00,0xa0,0xc9,0x22,0xe6,0xeb}},
      8, slots_IDirectMusicSegmentState, 0 },
    { "IDirectMusicTool", {0xd2ac28ba,0xb39b,0x11d1,{0x87,0x04,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      9, slots_IDirectMusicTool, 0 },
    { "IDirectMusicTrack", {0xf96029a1,0x4282,0x11d2,{0x87,0x17,0x00,0x60,0x08,0x93,0xb1,0xbd}},
      13, slots_IDirectMusicTrack, 0 },
    { "IDirectSound", {0x279afa83,0x4981,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}},
      11, slots_IDirectSound, 0 },
    { "IDirectSoundBuffer", {0x279afa85,0x4981,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}},
      21, slots_IDirectSoundBuffer, 0 },
    { "IDispatch", {0x00020400,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IDispatch, 0 },
    { "IEnumConnectionPoints", {0xb196b285,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07}},
      7, slots_IEnumConnectionPoints, 0 },
    { "IEnumConnections", {0xb196b287,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07}},
      7, slots_IEnumConnections, 0 },
    { "IEnumMoniker", {0x00000102,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IEnumMoniker, 0 },
    { "IEnumSTATSTG", {0x0000000d,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IEnumSTATSTG, 0 },
    { "IEnumString", {0x00000101,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IEnumString, 0 },
    { "IEnumUnknown", {0x00000100,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      7, slots_IEnumUnknown, 0 },
    { "IErrorInfo", {0x1cf2b120,0x547d,0x101b,{0x8e,0x65,0x08,0x00,0x2b,0x2b,0xd1,0x19}},
      8, slots_IErrorInfo, 0 },
    { "IGlobalInterfaceTable", {0x00000146,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      6, slots_IGlobalInterfaceTable, 0 },
    { "IInspectable", {0xaf86e2e0,0xb12d,0x4c6a,{0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90}},
      6, slots_IInspectable, 0 },
    { "ILockBytes", {0x0000000a,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      10, slots_ILockBytes, 0 },
    { "IMMDevice", {0xd666063f,0x1587,0x4e43,{0x81,0xf1,0xb9,0x48,0xe8,0x07,0x36,0x3f}},
      7, slots_IMMDevice, 0 },
    { "IMMDeviceCollection", {0x0bd7a1be,0x7a1a,0x44db,{0x83,0x97,0xcc,0x53,0x92,0x38,0x7b,0x5e}},
      5, slots_IMMDeviceCollection, 0 },
    { "IMMDeviceEnumerator", {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}},
      8, slots_IMMDeviceEnumerator, 0 },
    { "IMMNotificationClient", {0x7991eec9,0x7e89,0x4d85,{0x83,0x90,0x6c,0x70,0x3c,0xec,0x60,0xc0}},
      8, slots_IMMNotificationClient, 0 },
    { "IMalloc", {0x00000002,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      9, slots_IMalloc, 0 },
    { "IMarshal", {0x00000003,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      9, slots_IMarshal, 0 },
    { "IMoniker", {0x0000000f,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      23, slots_IMoniker, 0 },
    { "IMultiQI", {0x00000020,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      4, slots_IMultiQI, 0 },
    { "IPersist", {0x0000010c,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      4, slots_IPersist, 0 },
    { "IPersistFile", {0x0000010b,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      9, slots_IPersistFile, 0 },
    { "IPersistStream", {0x00000109,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      8, slots_IPersistStream, 0 },
    { "IPersistStreamInit", {0x7fd52380,0x4e07,0x101b,{0xae,0x2d,0x08,0x00,0x2b,0x2e,0xc7,0x13}},
      9, slots_IPersistStreamInit, 0 },
    { "IRecordInfo", {0x0000002f,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      19, slots_IRecordInfo, 0 },
    { "IReferenceClock", {0x56a86897,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}},
      7, slots_IReferenceClock, 0 },
    { "IRunningObjectTable", {0x00000010,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      10, slots_IRunningObjectTable, 0 },
    { "ISequentialStream", {0x0c733a30,0x2a1c,0x11ce,{0xad,0xe5,0x00,0xaa,0x00,0x44,0x77,0x3d}},
      5, slots_ISequentialStream, 0 },
    { "IStorage", {0x0000000b,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      18, slots_IStorage, 0 },
    { "IStream", {0x0000000c,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      14, slots_IStream, 0 },
    { "ISupportErrorInfo", {0xdf0b3d60,0x548f,0x101b,{0x8e,0x65,0x08,0x00,0x2b,0x2b,0xd1,0x19}},
      4, slots_ISupportErrorInfo, 0 },
    { "ITypeComp", {0x00020403,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      5, slots_ITypeComp, 0 },
    { "ITypeInfo", {0x00020401,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      22, slots_ITypeInfo, 0 },
    { "ITypeLib", {0x00020402,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      13, slots_ITypeLib, 0 },
    { "IUnknown", {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      3, slots_IUnknown, 0 },
    { "IXAudio2", {0x8bcf1f58,0x9fe7,0x4583,{0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb}},
      16, slots_IXAudio2, 0 },
    { "IXAudio2EngineCallback", {0xe052c39d,0xed73,0x58c6,{0xac,0xd9,0x76,0x0e,0x2c,0x8f,0x79,0x68}},
      3, slots_IXAudio2EngineCallback, WINECOM_IF_LOCAL },
    { "IXAudio2MasteringVoice", {0x359298ea,0xdc9b,0x572f,{0x82,0xc6,0xcf,0xd2,0x05,0x11,0xf2,0x15}},
      19, slots_IXAudio2MasteringVoice, WINECOM_IF_LOCAL },
    { "IXAudio2SourceVoice", {0xe8116f61,0xbcd1,0x5163,{0x8e,0x67,0xf6,0x84,0x4b,0x28,0xf3,0x7e}},
      29, slots_IXAudio2SourceVoice, WINECOM_IF_LOCAL },
    { "IXAudio2SubmixVoice", {0x735f90fc,0xf8c2,0x5c07,{0x9c,0xb0,0x67,0xd0,0x6f,0xe8,0x28,0x27}},
      19, slots_IXAudio2SubmixVoice, WINECOM_IF_LOCAL },
    { "IXAudio2Voice", {0x76c75fa9,0x5832,0x597c,{0x96,0xae,0x87,0x30,0xae,0x5f,0xab,0x3d}},
      19, slots_IXAudio2Voice, WINECOM_IF_LOCAL },
};

/* Interfaces that are NOT IUnknown-derived: slot 0 is a real method, not
 * QueryInterface.  libs/winecom's dispatcher serves slots 0..2 from the proxy
 * table for every interface it is given, so combase's __wine_com_dispatch MUST
 * test this array and serve these itself before delegating. */
static const unsigned char syscom_iface_local[SYSCOM_IFACE_COUNT] =
{
    0,  /* IActivationFactory */
    0,  /* IAgileObject */
    0,  /* IAudioClient */
    0,  /* IAudioRenderClient */
    0,  /* IBindCtx */
    0,  /* IClassFactory */
    0,  /* IConnectionPoint */
    0,  /* IConnectionPointContainer */
    0,  /* ICreateErrorInfo */
    0,  /* IDirectMusic */
    0,  /* IDirectMusicAudioPath */
    0,  /* IDirectMusicBand */
    0,  /* IDirectMusicBuffer */
    0,  /* IDirectMusicDownloadedInstrument */
    0,  /* IDirectMusicGetLoader */
    0,  /* IDirectMusicGraph */
    0,  /* IDirectMusicInstrument */
    0,  /* IDirectMusicLoader */
    0,  /* IDirectMusicLoader8 */
    0,  /* IDirectMusicObject */
    0,  /* IDirectMusicPerformance */
    0,  /* IDirectMusicPerformance8 */
    0,  /* IDirectMusicPort */
    0,  /* IDirectMusicSegment */
    0,  /* IDirectMusicSegment8 */
    0,  /* IDirectMusicSegmentState */
    0,  /* IDirectMusicTool */
    0,  /* IDirectMusicTrack */
    0,  /* IDirectSound */
    0,  /* IDirectSoundBuffer */
    0,  /* IDispatch */
    0,  /* IEnumConnectionPoints */
    0,  /* IEnumConnections */
    0,  /* IEnumMoniker */
    0,  /* IEnumSTATSTG */
    0,  /* IEnumString */
    0,  /* IEnumUnknown */
    0,  /* IErrorInfo */
    0,  /* IGlobalInterfaceTable */
    0,  /* IInspectable */
    0,  /* ILockBytes */
    0,  /* IMMDevice */
    0,  /* IMMDeviceCollection */
    0,  /* IMMDeviceEnumerator */
    0,  /* IMMNotificationClient */
    0,  /* IMalloc */
    0,  /* IMarshal */
    0,  /* IMoniker */
    0,  /* IMultiQI */
    0,  /* IPersist */
    0,  /* IPersistFile */
    0,  /* IPersistStream */
    0,  /* IPersistStreamInit */
    0,  /* IRecordInfo */
    0,  /* IReferenceClock */
    0,  /* IRunningObjectTable */
    0,  /* ISequentialStream */
    0,  /* IStorage */
    0,  /* IStream */
    0,  /* ISupportErrorInfo */
    0,  /* ITypeComp */
    0,  /* ITypeInfo */
    0,  /* ITypeLib */
    0,  /* IUnknown */
    0,  /* IXAudio2 */
    1,  /* IXAudio2EngineCallback */
    1,  /* IXAudio2MasteringVoice */
    1,  /* IXAudio2SourceVoice */
    1,  /* IXAudio2SubmixVoice */
    1,  /* IXAudio2Voice */
};

/* wine-syscom: 70 interface(s), 805 vtable slot(s).
 * The 12 audio row(s) generated here: 111 slot(s) marshalled, 19 hand-written
 * (5 of them float-bearing, routed by argument shape), 2 refused with a
 * named reason, 21 IUnknown slot(s) served by the runtime, 5 interface(s)
 * [local] and served by combase's own dispatcher.  The 58 reused row(s):
 * 365 marshalled, 113 refused, 174 IUnknown; 361 of them re-derived from
 * the roster and cross-checked against this file.
 * Reverse-proxy licence: IMMNotificationClient, IXAudio2EngineCallback.  121 interface IN-parameter(s)
 * withheld, each of which fails closed. */
