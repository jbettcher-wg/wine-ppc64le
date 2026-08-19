/* sl.interposer.dll -- the NO-STREAMLINE build for AMD hardware.
 *
 * Staged into a prefix's guest system directory, where find_dll_file()'s
 * guest branch finds it BEFORE the game's own copy.  NVIDIA Streamline's
 * real interposer detours D3D12/DXGI entry points with copied-code
 * trampolines; on this port those trampolines copy TRAP STUBS, and
 * sl.reflex.dll then calls instruction bytes as a pointer (measured:
 * Cyberpunk 2077, wild pointer 518B4908488D4810 = `push rcx; mov ...`).
 * On an AMD GPU every Streamline feature is unsupported anyway, so the
 * honest answer is an interposer that says so: the D3D/DXGI creators pass
 * straight through to the real modules and every sl* entry point reports
 * failure, which the game's own Streamline error handling turns into
 * "DLSS/Reflex unavailable".
 */
#include <windows.h>

static FARPROC lazy( const char *mod, const char *name )
{
    HMODULE m = LoadLibraryA( mod );
    return m ? GetProcAddress( m, name ) : NULL;
}

#define FWD( retty, name, mod, proto, args )                         \
    retty WINAPI name proto                                          \
    {                                                                \
        static retty (WINAPI *p) proto;                              \
        if (!p) p = (retty (WINAPI *) proto)lazy( mod, #name );      \
        if (!p) return (retty)0x80004005;  /* E_FAIL */              \
        return p args;                                               \
    }

FWD( int, D3D12CreateDevice, "d3d12.dll",
     ( void *a, int b, const GUID *c, void **d ), ( a, b, c, d ) )
FWD( int, D3D12GetDebugInterface, "d3d12.dll",
     ( const GUID *a, void **b ), ( a, b ) )
FWD( int, D3D12SerializeRootSignature, "d3d12.dll",
     ( const void *a, int b, void **c, void **d ), ( a, b, c, d ) )
FWD( int, D3D12SerializeVersionedRootSignature, "d3d12.dll",
     ( const void *a, void **b, void **c ), ( a, b, c ) )
FWD( int, CreateDXGIFactory, "dxgi.dll",
     ( const GUID *a, void **b ), ( a, b ) )
FWD( int, CreateDXGIFactory2, "dxgi.dll",
     ( unsigned int f, const GUID *a, void **b ), ( f, a, b ) )
FWD( int, D3D11CreateDeviceAndSwapChain, "d3d11.dll",
     ( void *a, int b, void *c, unsigned int d, const void *e, unsigned int f,
       unsigned int g, const void *h, void **i, void **j, void **k, void **l ),
     ( a, b, c, d, e, f, g, h, i, j, k, l ) )

/* sl::Result is 0 for eOk; anything else is a failure the SDK's own
 * SL_FAILED() macro catches.  1 == eErrorIO in every Streamline release;
 * the exact member matters less than being reliably nonzero. */
#define SL_STUB( name )  int WINAPI name( void ) { return 1; }
/* The callers pass arguments; x64 is caller-clean, so a zero-parameter
 * definition answers any arity. */
SL_STUB( slInit )
SL_STUB( slShutdown )
SL_STUB( slIsFeatureSupported )
SL_STUB( slIsFeatureLoaded )
SL_STUB( slSetFeatureLoaded )
SL_STUB( slGetFeatureFunction )
SL_STUB( slGetNewFrameToken )
SL_STUB( slSetConstants )
SL_STUB( slSetTag )
SL_STUB( slSetD3DDevice )
SL_STUB( slUpgradeInterface )
SL_STUB( slAllocateResources )
SL_STUB( slFreeResources )
SL_STUB( slGetFeatureRequirements )
SL_STUB( slGetFeatureVersion )
SL_STUB( slGetNativeInterface )

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    return TRUE;
}
