# The guest COM boundary (guestcom.c, shared with d3dcompiler_47 via
# PARENTSRC; see that module's .spec for the contract).
@ stdcall __wine_com_dispatch(long long ptr)
@ stdcall __wine_com_slot_name(long long ptr ptr)
@ stdcall __wine_com_refuse()
@ stdcall -private __wine_guest_D3DCompile(ptr long str ptr ptr str str long long ptr ptr)
@ stdcall -private __wine_guest_D3DCreateBlob(long ptr)
@ stdcall -private __wine_guest_D3DDisassemble(ptr long long ptr ptr)
@ stdcall -private __wine_guest_D3DGetBlobPart(ptr long long long ptr)
@ stdcall -private __wine_guest_D3DGetDebugInfo(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetInputAndOutputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetInputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DGetOutputSignatureBlob(ptr long ptr)
@ stdcall -private __wine_guest_D3DPreprocess(ptr long str ptr ptr ptr ptr)
@ stdcall -private __wine_guest_D3DStripShader(ptr long long ptr)
@ stdcall -private D3DAssemble(ptr long str ptr ptr long ptr ptr)
@ stub DebugSetMute
@ stdcall D3DCompile(ptr long str ptr ptr str str long long ptr ptr)
@ stub D3DCompressShaders
@ stdcall D3DCreateBlob(long ptr)
@ stub D3DDecompressShaders
@ stub D3DDisassemble10Effect(ptr long ptr)
@ stdcall D3DDisassemble(ptr long long ptr ptr)
@ stdcall D3DGetBlobPart(ptr long long long ptr)
@ stdcall D3DGetDebugInfo(ptr long ptr)
@ stdcall D3DGetInputAndOutputSignatureBlob(ptr long ptr)
@ stdcall D3DGetInputSignatureBlob(ptr long ptr)
@ stdcall D3DGetOutputSignatureBlob(ptr long ptr)
@ stdcall D3DPreprocess(ptr long str ptr ptr ptr ptr)
@ stdcall D3DReflect(ptr long ptr ptr)
@ stub D3DReturnFailure1
@ stdcall D3DStripShader(ptr long long ptr)
