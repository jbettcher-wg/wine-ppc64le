@ stdcall DirectInputCreateA(long long ptr ptr)
@ stdcall DirectInputCreateEx(long long ptr ptr ptr)
@ stdcall DirectInputCreateW(long long ptr ptr)
@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()

# The guest-side entry points for the three creators above.  An x86-64 guest's
# DirectInputCreateA/W/Ex resolves HERE (tools/spec2thunk GUEST-IMPL, named in
# dinput.thunks) rather than at the plain name, which would hand it an
# IDirectInput whose vtable is native ppc64 code.  See
# dlls/dinput/guestthunk.c.
@ stdcall __wine_guest_DirectInputCreateA(long long ptr ptr)
@ stdcall __wine_guest_DirectInputCreateEx(long long ptr ptr ptr)
@ stdcall __wine_guest_DirectInputCreateW(long long ptr ptr)
