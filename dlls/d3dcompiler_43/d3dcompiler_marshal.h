/* HAND-WRITTEN to ppc64le/shell/interfaces_d3dcompiler.json -- keep the two
 * in step BY HAND, and the reason there is no generator: the roster is one
 * real interface with two parameterless methods, and ID3D10Blob is declared
 * by a widl-generated header (d3dcommon.h) that neither the DECLARE_INTERFACE_
 * parser (gen_dinput_surface.py) nor the DXVK vendored-header parser reads.
 * A five-slot table is smaller than a third parser.  libs/winecom still
 * cross-checks every IID and slot count against the guest thunk module at
 * attach -- spec2thunk COM mode reads the SAME JSON -- so a drift between
 * this file and the JSON is a loud attach failure, not a runtime mystery.
 *
 * Interface order is sorted by name, which is the order spec2thunk COM mode
 * gives the guest stub arrays.
 *
 * Slot/iface types and WINECOM_CA_* classes come from include/wine/winecom.h,
 * which must be included before this file.
 */

enum d3dcompiler_iface_index
{
    D3DCOMPILER_IFACE_ID3D10Blob = 0,
    D3DCOMPILER_IFACE_IUnknown = 1,
    D3DCOMPILER_IFACE_COUNT = 2
};

#define D3DCOMPILER_HAND_COUNT 0

/* GetBufferPointer's return value is a HOST heap pointer and crosses as
 * plain data ON PURPOSE: the guest and the host share one address space on
 * this port, so the guest READS the blob bytes directly; only CODE pointers
 * (vtables) must never cross raw, and those are exactly what the proxy this
 * table serves exists to replace. */
static const struct winecom_slot slots_ID3D10Blob[5] =
{
    { "IUnknown::QueryInterface", NULL, NULL, NULL, 3, 0, 0, 0, NULL, 0, 0, 0 },
    { "IUnknown::AddRef", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0, 0, 0 },
    { "IUnknown::Release", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0, 0, 0 },
    { "ID3D10Blob::GetBufferPointer", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0, 0, 0 },
    { "ID3D10Blob::GetBufferSize", NULL, NULL, NULL, 1, 0, 0, 0, NULL, 0, 0, 0 },
};

static const struct winecom_iface d3dcompiler_com_ifaces[D3DCOMPILER_IFACE_COUNT] =
{
    { "ID3D10Blob", {0x8ba5fb08,0x5195,0x40e2,{0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}},
      5, slots_ID3D10Blob, 0 },
    { "IUnknown", {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}},
      3, NULL, 0 },
};

/*
 * 2 slot(s) marshalled, 0 hand-written, 0 refused with a named reason, 6
 * IUnknown slot(s) served by the runtime.
 */
