@ stdcall DWriteCreateFactory(long ptr ptr)

# The guest-side entry point for the export above: an x86-64 guest's
# DWriteCreateFactory resolves HERE (tools/spec2thunk GUEST-IMPL, named in
# dwrite.thunks) rather than at the plain name, because the plain one vends an
# interface whose vtable is native ppc64 code.  See dlls/dwrite/guestthunk.c.
@ stdcall __wine_guest_DWriteCreateFactory(long ptr ptr)
