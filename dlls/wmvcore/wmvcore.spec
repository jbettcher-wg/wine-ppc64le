@ stdcall WMCheckURLExtension(wstr)
@ stdcall WMCheckURLScheme(wstr)
@ stdcall WMCreateBackupRestorerPrivate(ptr ptr) WMCreateBackupRestorer
@ stub WMIsAvailableOffline
@ stub WMValidateData
@ stdcall -private DllRegisterServer()
@ stdcall WMCreateBackupRestorer(ptr ptr)
@ stdcall WMCreateEditor(ptr)
@ stub WMCreateIndexer
@ stdcall WMCreateProfileManager(ptr)
@ stdcall WMCreateReader(ptr long ptr)
@ stdcall WMCreateReaderPriv(ptr)
@ stdcall WMCreateSyncReader(ptr long ptr)
@ stdcall WMCreateSyncReaderPriv(ptr)
@ stdcall WMCreateWriter(ptr ptr)
@ stub WMCreateWriterFileSink
@ stub WMCreateWriterNetworkSink
@ stdcall WMCreateWriterPriv(ptr)
@ stub WMCreateWriterPushSink
@ stdcall WMIsContentProtected(wstr ptr)

# Windows Media for x86-64 guests (ppc64le/mf/README.md).  The ONE winecom
# instance for this surface lives in mfplat.dll, so the dispatcher forwards
# there; the wrappers are dlls/wmvcore/mfcom.c.
@ stdcall __wine_com_dispatch(long long ptr) mfplat.__wine_com_dispatch
@ stdcall __wine_guest_WMCreateReader(ptr long ptr)
@ stdcall __wine_guest_WMCreateSyncReader(ptr long ptr)
@ stdcall __wine_guest_WMCreateWriter(ptr ptr)
@ stdcall __wine_guest_WMCreateProfileManager(ptr)
@ stdcall __wine_guest_WMCreateEditor(ptr)
@ stdcall __wine_guest_WMCreateBackupRestorer(ptr ptr)

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr) mfplat.__wine_com_slot_name
