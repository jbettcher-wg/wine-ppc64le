# Shared-resource HANDLEs across the Wine↔DXVK boundary — the design owed

The rows this note backs (refused-with-this-citation in d3d11_marshal.h):
`ID3D10Device::OpenSharedResource`, `ID3D11Device::OpenSharedResource`,
`ID3D11Device1::OpenSharedResource1`, `ID3D11Device1::OpenSharedResourceByName`,
`ID3D11Device5::OpenSharedFence`, `ID3D11Fence::CreateSharedHandle`,
`IDXGIResource1::CreateSharedHandle`, and IDXGIFactoryMedia's two
composition-surface routes (doubly dead: DXVK implements no IDXGIFactoryMedia
at all).

## Why these are refused today, precisely

Events were the EASY half of the by-value-HANDLE class and are served now:
an event is a signal, so one eventfd plus the 'EVFD' tag carries its whole
meaning across (the winecom event relay, 2026-09-01).  A shared RESOURCE
handle is a name for **memory**: DXVK's native side resolves it through its
own D3DKMT emulation — `OpenSharedResource` calls `D3DKMTQueryResourceInfo`
/ `D3DKMTOpenResource` over kmt-style global-share integers (it even
enforces the 0xc0000000 kmt tag bits before trying,
src/d3d11/d3d11_device.cpp:1445), expecting a resource that was *created*
in the same kmt emulation by another DXVK (or vkd3d) instance, with private
runtime data describing the allocation.

A Wine shared-resource HANDLE names a wineserver object with (today) no
GPU-memory export behind it on this lane.  There is nothing an integer
translation can do — unlike events, the two sides do not share a substrate
yet.

## What the seam has to be

1. **Producer side**: when a Wine process exports a resource
   (`CreateSharedHandle` / the legacy `GetSharedHandle`), the underlying
   VkDeviceMemory must be exported (`VK_KHR_external_memory_fd`, opaque fd)
   and the fd parked under the wineserver object the HANDLE names — the
   ntsync precedent: the HANDLE stays a Wine handle, the payload is an fd.
2. **Consumer side**: `OpenSharedResource*` resolves the Wine HANDLE to that
   fd (server request, dup semantics like `get_handle_fd`), then hands DXVK
   an import path.  DXVK's import expects its kmt namespace, so either
   (a) a small DXVK patch adding an fd-import route beside the kmt one
   (the tagged-event patch 0006 is the template: recognise a tag, take the
   native path), or (b) registering the imported allocation INTO DXVK's kmt
   emulation so the existing code path serves it unchanged.  (b) touches
   less API surface; (a) is more honest about what is happening.  Decide
   when building.
3. **Cross-lane**: a d3d12 (vkd3d) producer and a d3d11 (DXVK) consumer —
   or the reverse — must agree on the payload format.  Both already agree
   on the event tag; the resource payload should be specified the same way
   (one header, respelled per project, gates asserting agreement).
4. **The fence variant** (`OpenSharedFence`): `VK_KHR_external_semaphore_fd`
   instead of memory; otherwise the same shape.

Until the seam exists the rows stay refused **and inert** (wave-1 scrub
masks null the out-params), and the refusal reason cites this file.  No row
in this class is refused for lack of a consumer — the blocker is the
unbuilt export/import substrate, and this note is the build plan.
