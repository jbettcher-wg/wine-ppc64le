@ stdcall DirectInput8Create(long long ptr ptr ptr)
@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()

# --------------------------------------------------------------------------
# The x86-64 guest boundary (dlls/dinput8/guestcom.c, dinput8.thunks).
#
#   __wine_com_dispatch    the single entry ntdll's COM trap dispatcher calls
#                          on this module when a guest touches a slot in one
#                          of the vtables the guest thunk publishes.  Contract
#                          in include/wine/winecom.h.
#   __wine_guest_*         what an emulated x86-64 guest reaches instead of the
#                          plain export, through spec2thunk's GUEST-IMPL
#                          redirect: the plain one vends an IDirectInput8 whose
#                          vtable is native ppc64 code.
#   __wine_com_refuse      the shared loud refusal every GUEST-REFUSE export
#                          resolves to.
# --------------------------------------------------------------------------
@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_guest_DirectInput8Create(long long ptr ptr ptr)
@ stdcall __wine_com_refuse()

# Appended at the END so no `@` export above it is renumbered:
# ordinals are assigned in file order and guests import by ordinal
# (ppc64le/vkd3d/check-ordinal-imports.sh).  Asked of the NATIVE
# module by ntdll when the crossing sink interns a COM slot row.
@ stdcall __wine_com_slot_name(long long ptr ptr)
