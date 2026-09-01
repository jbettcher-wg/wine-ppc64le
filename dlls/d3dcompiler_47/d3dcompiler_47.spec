# The guest COM boundary (guestcom.c; see dlls/dinput8/dinput8.spec for the
# contract): __wine_com_dispatch is the single entry ntdll's COM trap
# dispatcher calls, __wine_guest_* is what an emulated x86-64 guest reaches
# instead of the plain export (spec2thunk GUEST-IMPL), __wine_com_refuse is
# the shared loud refusal every GUEST-REFUSE export resolves to.
@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_com_slot_name(long long ptr ptr)
@ stdcall __wine_com_refuse()
@ stdcall -private __wine_guest_D3DCompile(ptr long str ptr ptr str str long long ptr ptr)
@ stdcall -private __wine_guest_D3DCompile2(ptr long str ptr ptr str str long long long ptr long ptr ptr)
@ stdcall -private __wine_guest_D3DCompileFromFile(wstr ptr ptr str str long long ptr ptr)
@ stdcall -private __wine_guest_D3DCreateBlob(long ptr)
@ stdcall -private __wine_guest_D3DDisassemble(ptr long long ptr ptr)
@ stdcall -private __wine_guest_D3DGetBlobPart(ptr long long long ptr)
@ stdcall -private __wine_guest_D3DGetDebugInfo(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetInputAndOutputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetInputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetOutputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DPreprocess(ptr long str ptr ptr ptr ptr)
@ stdcall -private __wine_guest_D3DReadFileToBlob(wstr ptr)
@ stdcall -private __wine_guest_D3DStripShader(ptr long long ptr)
@ stdcall -private __wine_guest_D3DWriteBlobToFile(ptr wstr long)
@ stdcall -private D3DAssemble(ptr long str ptr ptr long ptr ptr)
@ stdcall D3DCompile(ptr long str ptr ptr str str long long ptr ptr)
@ stdcall D3DCompile2(ptr long str ptr ptr str str long long long ptr long ptr ptr)
@ stdcall D3DCompileFromFile(wstr ptr ptr str str long long ptr ptr)
@ stub D3DCompressShaders
@ stdcall D3DCreateBlob(long ptr)
@ stdcall D3DCreateFunctionLinkingGraph(long ptr)
@ stdcall D3DCreateLinker(ptr)
@ stub D3DDecompressShaders
@ stdcall D3DDisassemble(ptr long long ptr ptr)
@ stub D3DDisassemble10Effect(ptr long ptr)
@ stub D3DDisassemble11Trace
@ stub D3DDisassembleRegion
@ stdcall D3DGetBlobPart(ptr long long long ptr)
@ stdcall D3DGetDebugInfo(ptr long ptr)
@ stdcall D3DGetInputAndOutputSignatureBlob(ptr long ptr)
@ stdcall D3DGetInputSignatureBlob(ptr long ptr)
@ stdcall D3DGetOutputSignatureBlob(ptr long ptr)
@ stub D3DGetTraceInstructionOffsets
@ stdcall D3DLoadModule(ptr long ptr)
@ stdcall D3DPreprocess(ptr long str ptr ptr ptr ptr)
@ stdcall D3DReadFileToBlob(wstr ptr)
@ stdcall D3DReflect(ptr long ptr ptr)
@ stub D3DReflectLibrary
@ stub D3DReturnFailure1
@ stub D3DSetBlobPart
@ stdcall D3DStripShader(ptr long long ptr)
@ stdcall D3DWriteBlobToFile(ptr wstr long)
@ stub DebugSetMute
