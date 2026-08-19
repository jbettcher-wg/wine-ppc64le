# The i386 half of the dxvk thunk surface — design and measured scope

Written 2026-08-19, the scoping session.  dexwin (Dex, PE32, Unity 5) is the
canary: it boots through the ABI-4 bridge, Unity puts its window up, and then
i386 dxgi.dll's forwards to `d3d11.__wine_dxvk_*` fail because the i386 build
of this port's d3d11 exports none of the dxvk surface.  Portal 2 and HL2 wait
behind the same wall.

## What the 64-bit lane does (the shape being mirrored)

```
guest x86-64 PE --> sysx8664\{d3d11,dxgi,d3d10core}.dll   (spec2thunk COM
   |                 mode: trap stubs, 5-byte `mov r10,rcx; syscall` at
   |                 16-byte stride, one array per interface,
   |                 __wine_com_thunk_info describes them)
   |  trap; ntdll maps RIP -> (iface, slot)
   v
native d3d11.dll __wine_com_dispatch( iface, slot, AMD64_CONTEXT* )
   |  libs/winecom dispatch over d3d11_marshal.h
   v
d3d11.so unixlib --> libdxvk_d3d11.so / libdxvk_dxgi.so
```

## The four pieces of the i386 half

1. **spec2thunk grows an i386 COM emitter.**  Stub body is `int 0x80`
   (`CD 80`) — the exact instruction this FEXCore build already routes into
   the same OS_GENERIC syscall sink the 64-bit lane's `0F 05` uses, RIP left
   at the site (dlls/ntdll/unix/loader.c, "The 32-bit (WoW64) lane" banner).
   Same per-interface arrays, same sorted-by-name order from the same JSON,
   an i386-flavored `__wine_com_thunk_info`.  Flat exports
   (D3D11CreateDevice &c.) get i386 trap stubs the same way — the 32-bit
   lane's OTHER modules are Wine's real i386 builds and need no stubs, but
   d3d11/dxgi/d3d10core REPLACE their implementations, same as on 64.

2. **The wow64 run loop learns COM stub RIPs.**  Every emu32 run is bounded;
   a bop returns to the PE-side loop with the guest context in hand
   (wow64cpu_ppc64.c).  Today the loop knows the two canonical bop sites;
   it gains the same exact-hit stub-range arithmetic ntdll's 64-bit
   dispatcher uses, answering (iface, slot) and calling the native
   namesake's 32-bit dispatch entry.

3. **libs/winecom grows dispatch32.**  The guest is stdcall: EVERY argument
   is a 4-byte stack slot at ESP+4+4n (the bounded-run context arrives as
   AMD64_CONTEXT via emu32_context_to_bridge, ESP in Rsp).  dispatch32
   widens into the same UINT64 args[] the 64-bit path builds and everything
   downstream — classes, xaux, invoke, the unixlib — is UNCHANGED:
     - scalars: zero-extend; `dwordsign` slots sign-extend (the table
       already says which);
     - pointers: zero-extend (wow64 guest lives below 4 GiB, so a widened
       guest pointer IS a valid host pointer);
     - interface-pointer ARRAYS (CA_IFACE_ARR_IN / _OUT_STATIC): the 4-byte
       elements widen into a scratch array on the way in and narrow on the
       way out — the count parameter the marshal table already names makes
       this generic, and it covers the hot per-frame calls
       (VSSetShaderResources, OMSetRenderTargets, SetConstantBuffers...);
     - the divergent structs below get hand walkers, the d3d12 pattern.

4. **Hand walkers for the divergent descriptor structs.**  Measured with
   `layout32` (this directory): compile the same 297 D3D11_/DXGI_ aggregates
   from Wine's own headers for x86_64-windows and i386-windows, diff
   size/align.  **47 of 297 diverge**, and they split:
     - ~35 are D3D11_AUTHENTICATED_* / VIDEO_DECODER_* / KEY_EXCHANGE_*
       content-protection and video surfaces no Unity-5/Source-era title
       calls.  REFUSE these by name on the 32-bit path until a title
       proves the need — the honest default this codebase already uses.
     - The dozen a real game needs, each a mechanical widen/narrow:
       D3D11_INPUT_ELEMENT_DESC, D3D11_SUBRESOURCE_DATA,
       D3D11_MAPPED_SUBRESOURCE (out!), D3D11_SO_DECLARATION_ENTRY,
       DXGI_SWAP_CHAIN_DESC, DXGI_ADAPTER_DESC/1/2/3, DXGI_OUTPUT_DESC/1,
       DXGI_MAPPED_RECT (out), DXGI_PRESENT_PARAMETERS,
       DXGI_SHARED_RESOURCE.
   The affected slots are the creation/query calls (CreateBuffer,
   CreateTexture*, CreateInputLayout, Map, CreateSwapChain*, GetDesc*,
   Present1, MapDesktopSurface) — cold paths, one walker each.

## What does NOT need doing

- No repack for the 250 layout-identical structs: guest pointer passes
  through, exactly as on 64.
- No changes to the marshal tables, the unixlib, or dxvk itself.
- No FEX work: `int 0x80` already reaches the sink.

## Verification

dexwin end to end: window -> D3D11 device -> Unity scene.  The d3d11-smoke
gate gains a 32-bit leg (same probe source, i386 build, wow64 lane), and the
layout32 scan joins the tree so the divergent list is re-derived rather than
trusted.
