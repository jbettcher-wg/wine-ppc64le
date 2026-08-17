@ stdcall ChooseColorA(ptr)
@ stdcall ChooseColorW(ptr)
@ stdcall ChooseFontA(ptr)
@ stdcall ChooseFontW(ptr)
@ stdcall CommDlgExtendedError()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stdcall FindTextA(ptr)
@ stdcall FindTextW(ptr)
@ stdcall GetFileTitleA(str ptr long)
@ stdcall GetFileTitleW(wstr ptr long)
@ stdcall GetOpenFileNameA(ptr)
@ stdcall GetOpenFileNameW(ptr)
@ stdcall GetSaveFileNameA(ptr)
@ stdcall GetSaveFileNameW(ptr)
@ stub LoadAlterBitmap
@ stdcall PageSetupDlgA(ptr)
@ stdcall PageSetupDlgW(ptr)
@ stdcall PrintDlgA(ptr)
@ stdcall PrintDlgExA(ptr)
@ stdcall PrintDlgExW(ptr)
@ stdcall PrintDlgW(ptr)
@ stdcall ReplaceTextA(ptr)
@ stdcall ReplaceTextW(ptr)
@ stub WantArrows
@ stub dwLBSubclass
@ stub dwOKSubclass

# The guest-side entry points for the dialogs above.  An x86-64 guest's
# GetOpenFileNameA (and its seventeen siblings) resolves HERE
# (tools/spec2thunk GUEST-IMPL, named in comdlg32.thunks) rather than at the
# plain name, because every one of these takes a struct with an application
# hook procedure inside it, and a callback carried inside a struct is the one
# shape ntdll's argument-position override table cannot describe.  Each
# wrapper passes the call straight through when no hook is requested, and
# swaps the hook for one of ntdll's guest-callback trampolines when one is.
# See dlls/comdlg32/guestthunk.c.
@ stdcall __wine_guest_ChooseColorA(ptr)
@ stdcall __wine_guest_ChooseColorW(ptr)
@ stdcall __wine_guest_ChooseFontA(ptr)
@ stdcall __wine_guest_ChooseFontW(ptr)
@ stdcall __wine_guest_FindTextA(ptr)
@ stdcall __wine_guest_FindTextW(ptr)
@ stdcall __wine_guest_GetOpenFileNameA(ptr)
@ stdcall __wine_guest_GetOpenFileNameW(ptr)
@ stdcall __wine_guest_GetSaveFileNameA(ptr)
@ stdcall __wine_guest_GetSaveFileNameW(ptr)
@ stdcall __wine_guest_PageSetupDlgA(ptr)
@ stdcall __wine_guest_PageSetupDlgW(ptr)
@ stdcall __wine_guest_PrintDlgA(ptr)
@ stdcall __wine_guest_PrintDlgExA(ptr)
@ stdcall __wine_guest_PrintDlgExW(ptr)
@ stdcall __wine_guest_PrintDlgW(ptr)
@ stdcall __wine_guest_ReplaceTextA(ptr)
@ stdcall __wine_guest_ReplaceTextW(ptr)
