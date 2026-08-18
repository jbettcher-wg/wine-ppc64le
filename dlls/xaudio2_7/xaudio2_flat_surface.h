/*
 * xaudio2_flat_surface.h -- the XAudio2 flat export Wine declares nowhere.
 *
 * The XAudio2 sibling of ppc64le/dxvk/dxvk_flat_surface_d3d9.h, and it exists
 * for the same reason: spec2thunk's clang oracle types every export by reading
 * Wine's real headers, and an export Wine ships in a .spec but declares in no
 * header comes back as "no declaration found in Wine headers" and is refused --
 * a fact about the tooling, not about the ABI.  This file is named by
 * PROBE-EXTRA in dlls/xaudio2_8/xaudio2_8.thunks and
 * dlls/xaudio2_9/xaudio2_9.thunks and appended to the oracle's translation
 * unit.
 *
 * ONE NAME, and it is checked rather than assumed.  dlls/xapofx1_5/
 * xapofx1_5.thunks already records the finding in as many words: Wine
 * implements CreateFX in dlls/xaudio2_7/xapofx.c but include/xapofx.h declares
 * only the CLSIDs it uses, never the function.  CreateAudioReverb and
 * CreateAudioVolumeMeter are NOT here -- include/xaudio2fx.h (widl's output
 * from xaudio2fx.idl) declares both, so the two .thunks files simply
 * PROBE-EXTRA that header and the oracle types them from Wine's own
 * declarations, which is always the better answer.
 *
 * WHY THE DECLARATION LIVES HERE AND NOT IN include/xapofx.h.  CreateFX has
 * TWO shapes and the module being compiled picks which: dlls/xaudio2_7/
 * xapofx.c defines the four-argument form under `#if XAUDIO2_VER >= 8` and a
 * two-argument form under `#ifdef XAPOFX1_VER`, and dlls/xapofx1_5/
 * xapofx1_5.spec really does say `CreateFX(ptr ptr)`.  The oracle's
 * translation unit carries neither define -- there is no .thunks directive for
 * one, only PROBE-INCLUDE/PROBE-EXTRA/INCLUDE-DIR -- so a declaration added to
 * the shared public header would have to guess, and whichever way it guessed
 * it would hand the OTHER module an arity its own .spec contradicts.  A header
 * named only by the two .thunks files that want the >= 8 shape cannot make
 * that mistake: xapofx1_5 does not include it and keeps its documented hole.
 *
 * CDECL, not WINAPI, matching xapofx.c's definition and the `@ cdecl` line in
 * both specs.  The initialisation-data pointer is `void *` because that is
 * what xapofx.c declares -- it is passed through to the effect's Initialize
 * and never dereferenced here.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <xapofx.h>

HRESULT CDECL CreateFX( REFCLSID clsid, IUnknown **out, void *initdata,
                        UINT32 initdata_bytes );
