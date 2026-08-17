@ stdcall D2D1CreateFactory(long ptr ptr ptr)
@ stdcall D2D1MakeRotateMatrix(float float float ptr)
@ stdcall D2D1MakeSkewMatrix(float float float float ptr)
@ stdcall D2D1IsMatrixInvertible(ptr)
@ stdcall D2D1InvertMatrix(ptr)
@ stdcall D2D1ConvertColorSpace(long long ptr)
@ stdcall D2D1CreateDevice(ptr ptr ptr)
@ stdcall D2D1CreateDeviceContext(ptr ptr ptr)
@ stdcall D2D1SinCos(float ptr ptr)
@ stdcall D2D1Tan(float)
@ stdcall D2D1Vec3Length(float float float)
@ stdcall D2D1ComputeMaximumScaleFactor(ptr)

# The guest-side entry points for the three exports above that carry a COM
# interface: an x86-64 guest's D2D1CreateFactory/CreateDevice/CreateDeviceContext
# resolve HERE (tools/spec2thunk GUEST-IMPL, named in d2d1.thunks) instead of at
# the plain name, which would hand it a vtable of the wrong machine's code.
# See dlls/d2d1/guestthunk.c.
@ stdcall __wine_guest_D2D1CreateFactory(long ptr ptr ptr)
@ stdcall __wine_guest_D2D1CreateDevice(ptr ptr ptr)
@ stdcall __wine_guest_D2D1CreateDeviceContext(ptr ptr ptr)
