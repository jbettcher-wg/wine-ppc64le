/*
 * The six OPENGL32 exports Wine implements in C and declares in no header.
 *
 * Hand-written, unlike opengl32_guest_ext.h beside it, because these are not
 * derived from anything: they are opengl32's own private duplicates of gdi32's
 * pixel-format entry points, which Windows' opengl32.dll exports and no SDK
 * header has ever declared.  Wine's opengl32.spec exports them for the same
 * reason, and defines them in wgl.c and thunks.c and nowhere else -- so
 * spec2thunk's signature oracle, which reads FunctionDecls out of clang's AST,
 * found nothing and refused all six.  Losing wglSwapBuffers and
 * wglSetPixelFormat costs a guest the ability to present or to choose a
 * format at all, which is the whole module.
 *
 * Each line below is the definition's own prototype, copied from the site
 * named after it, and clang checks it against nothing -- so if one of these
 * ever disagrees with its definition, the disagreement is here.  The .spec
 * arity cross-check still runs (opengl32.spec pins two, three, one, four
 * arguments and so on), which catches the shape but not the return type.
 * Prefer a real header the moment one exists.
 *
 * This is the same device dlls/d3d11's dxvk_flat_surface.h uses for the nine
 * private entry points Wine ships in a .spec and declares nowhere.
 */

#ifndef __WINE_OPENGL32_GUEST_WGL_H
#define __WINE_OPENGL32_GUEST_WGL_H

#include <windef.h>
#include <wingdi.h>

INT  WINAPI wglChoosePixelFormat( HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd );          /* wgl.c:1302 */
INT  WINAPI wglDescribePixelFormat( HDC hdc, int index, UINT size,
                                    PIXELFORMATDESCRIPTOR *ppfd );                        /* wgl.c:1977 */
PROC WINAPI wglGetDefaultProcAddress( LPCSTR name );                                      /* wgl.c:2254 */
INT  WINAPI wglGetPixelFormat( HDC hdc );                                                 /* wgl.c:2071 */
BOOL WINAPI wglSetPixelFormat( HDC hdc, int ipfd, const PIXELFORMATDESCRIPTOR *ppfd );    /* thunks.c:22 */
BOOL WINAPI wglSwapBuffers( HDC hdc );                                                    /* wgl.c:2090 */

#endif /* __WINE_OPENGL32_GUEST_WGL_H */
