#!/usr/bin/env python3
"""
gen-guest-surface.py -- the guest OPENGL32 surface, derived, never written.

WHY THIS FILE EXISTS AT ALL.  Every other guest thunk module in this tree is
generated from the module's own Wine .spec, because a .spec IS by construction
the set of names a guest could import (tools/spec2thunk, "THE EXPORT LIST COMES
FROM THE MODULE'S OWN WINE .spec").  opengl32 is the one module where that is
not the whole truth, and necessarily so:

    Microsoft's opengl32.dll exports GL 1.1 and wgl* AND NOTHING ELSE.  It has
    exported exactly that since 1996.  Every entry point added to OpenGL since
    -- which is to say all of modern OpenGL, all 2756 of the entry points a
    2016 game actually draws with -- is vended at RUNTIME by wglGetProcAddress
    and appears in no export table anywhere.

Wine's opengl32.spec says the same 361 names, correctly.  So a guest thunk
built from it alone would let DOOM (2016) resolve its imports and then hand it
NULL for every single function it draws with.  A guest calling
wglGetProcAddress must get back a GUEST-CALLABLE stub, and a stub has to exist
before it can be handed out -- so the guest module's surface is the .spec's 361
names PLUS one stub per entry point Wine's own opengl32 can vend.

THE TWO OUTPUTS, AND WHERE EVERY BYTE OF THEM COMES FROM.

  dlls/opengl32/opengl32-guest.spec
      The export list spec2thunk reads (FROM-SPEC).  Two parts:
        * dlls/opengl32/opengl32.spec, copied LINE FOR LINE.  Verbatim, so
          this is a derived artifact and not a second list to maintain, and so
          the first 361 ordinals are numerically identical to the real
          module's -- a guest importing opengl32 by ordinal gets the same
          function the native module answers with.
        * one `@ stdcall` line per entry in dlls/opengl32/thunks.c's
          `extension_registry`, which is precisely the table Wine's own
          wglGetProcAddress answers from.  Serving a name that table does not
          have would be a stub with nothing behind it; serving fewer would be
          a hole a game falls into.

  dlls/opengl32/opengl32_guest_ext.h
      Prototypes for those same entry points, so spec2thunk's clang oracle can
      READ THE SIGNATURE rather than be told it.  include/wine/wgl.h declares
      every extension entry point as a `PFN_gl*` FUNCTION POINTER TYPEDEF and
      never as a function, because nothing in Wine calls them by name -- so
      this file restates each typedef as the declaration it implies.  The
      oracle then types the parameters through Wine's real typedef chain
      exactly as it does for every other module, and refuses what it cannot
      carry.  Nothing here asserts a signature; this file only makes the one
      that already exists visible to clang.

BOTH INPUTS ARE THEMSELVES GENERATED, FROM THE SAME PLACE.  wgl.h and thunks.c
are both written by dlls/opengl32/make_opengl from the Khronos OpenGL registry
XML.  So the .spec's argument classes and the header's parameter types are not
two independent readings, and the arity cross-check spec2thunk performs between
them is correspondingly weak for these entries -- said here rather than left
for someone to discover.  What still holds fully is the oracle's own verdict:
a shape it cannot carry across MS-x64 -> ELFv2 is refused by name, and the
generated .spec cannot talk it out of that.

USAGE
    ppc64le/opengl/gen-guest-surface.py            # regenerate in place
    ppc64le/opengl/gen-guest-surface.py --check    # exit 1 if either output
                                                   # has drifted from its
                                                   # inputs (the gate runs it)
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, '..', '..'))

WGL_H = os.path.join(SRC, 'include', 'wine', 'wgl.h')
THUNKS_C = os.path.join(SRC, 'dlls', 'opengl32', 'thunks.c')
BASE_SPEC = os.path.join(SRC, 'dlls', 'opengl32', 'opengl32.spec')
OUT_SPEC = os.path.join(SRC, 'dlls', 'opengl32', 'opengl32-guest.spec')
OUT_HDR = os.path.join(SRC, 'dlls', 'opengl32', 'opengl32_guest_ext.h')

GENERATOR = 'ppc64le/opengl/gen-guest-surface.py'

# --------------------------------------------------------------------------
# ARGUMENT CLASSES.  A Wine .spec states the CLASS of each argument, and
# spec2thunk uses that as a cross-check on what clang read -- so every type
# here is written down deliberately and an unknown one is a hard failure, not
# a `long`.  Widths are the ones include/wine/wgl.h gives on an LP64 host with
# a win64 PE guest, both of which make a pointer and an INT_PTR 64 bits.
# --------------------------------------------------------------------------
SPEC_CLASS = {
    # 32-bit integers
    'GLenum': 'long', 'GLuint': 'long', 'GLint': 'long', 'GLsizei': 'long',
    'GLboolean': 'long', 'GLbitfield': 'long', 'GLshort': 'long',
    'GLushort': 'long', 'GLbyte': 'long', 'GLubyte': 'long',
    'GLfixed': 'long', 'GLclampx': 'long', 'GLhalfNV': 'long',
    'GLhandleARB': 'long', 'GLchar': 'long', 'GLcharARB': 'long',
    'GLhalf': 'long', 'GLhalfARB': 'long',
    'int': 'long', 'unsigned int': 'long', 'UINT': 'long', 'INT': 'long',
    'BOOL': 'long', 'DWORD': 'long', 'COLORREF': 'long', 'float': 'float',
    # 64-bit integers.  INT_PTR on win64 and on ppc64le alike.
    'GLint64': 'int64', 'GLuint64': 'int64', 'GLint64EXT': 'int64',
    'GLuint64EXT': 'int64', 'GLintptr': 'int64', 'GLsizeiptr': 'int64',
    'GLintptrARB': 'int64', 'GLsizeiptrARB': 'int64',
    'GLvdpauSurfaceNV': 'int64', 'GLtime': 'int64',
    # floating point
    'GLfloat': 'float', 'GLclampf': 'float', 'FLOAT': 'float',
    'GLdouble': 'double', 'GLclampd': 'double', 'double': 'double',
    'DOUBLE': 'double',
    # pointers wearing a name
    'GLsync': 'ptr', 'GLeglImageOES': 'ptr', 'GLeglClientBufferEXT': 'ptr',
    'GLDEBUGPROC': 'ptr', 'GLDEBUGPROCAMD': 'ptr', 'GLDEBUGPROCARB': 'ptr',
    'GLDEBUGPROCKHR': 'ptr',
    'HDC': 'ptr', 'HGLRC': 'ptr', 'HPBUFFERARB': 'ptr', 'HPBUFFEREXT': 'ptr',
    'HANDLE': 'ptr', 'HWND': 'ptr', 'HBITMAP': 'ptr', 'HVIDEOOUTPUTDEVICENV': 'ptr',
    'HPVIDEODEV': 'ptr', 'HVIDEOINPUTDEVICENV': 'ptr', 'HGPUNV': 'ptr',
    'PROC': 'ptr', 'LPVOID': 'ptr', 'LPCSTR': 'str',
}

# The three entry points whose FIRST ARGUMENT IS A GUEST FUNCTION POINTER that
# native GL then calls back, once per debug message, for the life of the
# context.  They are excluded from the guest surface rather than served,
# because serving them pass-through would hand the driver a guest address to
# `bctrl` into -- x86-64 bytes decoded as ppc64 -- and the port's callback
# trampoline pool carries FOUR arguments while a GLDEBUGPROC takes SEVEN.
# wglGetProcAddress answers NULL for these and says so once, by name; a NULL
# from wglGetProcAddress is an answer every GL loader already handles, and a
# raw guest pointer is a crash none of them can diagnose.
CALLBACK_REFUSALS = {
    'glDebugMessageCallback':    'GLDEBUGPROC callback: 7 arguments, pool carries 4',
    'glDebugMessageCallbackARB': 'GLDEBUGPROCARB callback: 7 arguments, pool carries 4',
    'glDebugMessageCallbackAMD': 'GLDEBUGPROCAMD callback: 5 arguments, pool carries 4',
}


def die(msg):
    sys.exit('gen-guest-surface: %s' % msg)


def split_params(text):
    """C parameter list -> list of parameter declarations."""
    text = text.strip()
    if text in ('', 'void'):
        return []
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


def spec_class(decl, name, what):
    """One parameter declaration -> its Wine .spec argument class."""
    decl = decl.strip()
    if '*' in decl or '[' in decl:
        return 'ptr'
    toks = decl.replace('const', ' ').split()
    if not toks:
        die('%s %s: empty parameter' % (what, name))
    # the last token is the parameter's own name unless there is only a type
    base = ' '.join(toks[:-1]) if len(toks) > 1 else toks[0]
    if base not in SPEC_CLASS:
        die('%s %s: no .spec class for parameter type %r -- add it to '
            'SPEC_CLASS deliberately rather than defaulting it' % (what, name, base))
    return SPEC_CLASS[base]


def read_registry():
    """-> the names Wine's own wglGetProcAddress can answer with, in order."""
    text = open(THUNKS_C).read()
    m = re.search(r'const struct registry_entry extension_registry\[\] =\n'
                  r'\{\n(.*?)\n\};', text, re.S)
    if not m:
        die('no extension_registry[] in %s' % THUNKS_C)
    names = re.findall(r'^\s*\{ "(\w+)",', m.group(1), re.M)
    if not names:
        die('extension_registry[] in %s parsed empty' % THUNKS_C)
    return names


def read_pfn_typedefs():
    """-> {name: (return spelling, parameter list spelling)} from wgl.h."""
    text = open(WGL_H).read()
    out = {}
    for m in re.finditer(r'typedef\s+(.+?)\s*\(GLAPIENTRY \*PFN_(\w+)\)\(\s*(.*?)\s*\);',
                         text):
        out[m.group(2)] = (m.group(1).strip(), m.group(3).strip())
    if not out:
        die('no PFN_* typedefs in %s' % WGL_H)
    return out


def build_spec(registry, pfn):
    base = open(BASE_SPEC).read()
    lines = []
    lines.append('# Automatically generated by %s; DO NOT EDIT.\n' % GENERATOR)
    lines.append('#\n')
    lines.append('# The GUEST opengl32 export surface: what an x86-64 PE may bind to, which\n')
    lines.append('# for this one module is larger than what the native module exports.  Read\n')
    lines.append('# the generator for why, and dlls/opengl32/opengl32.thunks for how it is\n')
    lines.append('# used.  This file is spec2thunk input only -- winebuild never sees it and\n')
    lines.append('# the native opengl32.dll is unchanged by it.\n')
    lines.append('#\n')
    lines.append('# Part 1: dlls/opengl32/opengl32.spec, verbatim, so ordinals 1..N are the\n')
    lines.append('# real module\'s own.\n')
    lines.append('\n')
    lines.append(base if base.endswith('\n') else base + '\n')
    lines.append('\n')
    lines.append('# Part 2: one entry per row of extension_registry[] in\n')
    lines.append('# dlls/opengl32/thunks.c -- the table Wine\'s wglGetProcAddress answers\n')
    lines.append('# from, and therefore exactly the set of names this port can vend a\n')
    lines.append('# guest-callable stub for.  Argument classes are derived from the\n')
    lines.append('# PFN_* typedef in include/wine/wgl.h; the signature spec2thunk actually\n')
    lines.append('# marshals with comes from clang reading opengl32_guest_ext.h.\n')
    lines.append('\n')
    n = 0
    for name in registry:
        if name in CALLBACK_REFUSALS:
            continue
        ret, params = pfn[name]
        classes = [spec_class(p, name, 'registry entry') for p in split_params(params)]
        lines.append('@ stdcall %s(%s)\n' % (name, ' '.join(classes)))
        n += 1
    return ''.join(lines), n


def build_header(registry, pfn):
    lines = []
    lines.append('/* Automatically generated by %s; DO NOT EDIT.\n' % GENERATOR)
    lines.append(' *\n')
    lines.append(' * The OpenGL extension entry points Wine\'s opengl32 can vend, declared as\n')
    lines.append(' * the FUNCTIONS THEY ARE.  include/wine/wgl.h declares each one only as a\n')
    lines.append(' * PFN_* function-pointer typedef, because nothing inside Wine calls them by\n')
    lines.append(' * name -- and spec2thunk\'s signature oracle reads FunctionDecls out of\n')
    lines.append(' * clang\'s AST, so with no declaration there is nothing to read and every\n')
    lines.append(' * one of them would be refused.  Each line below restates one typedef as\n')
    lines.append(' * the declaration it implies, with the same types spelled the same way, so\n')
    lines.append(' * the oracle resolves them through Wine\'s own typedef chain.\n')
    lines.append(' *\n')
    lines.append(' * This header asserts nothing.  It is not a second source for these\n')
    lines.append(' * signatures; it is the one source, made visible to clang.\n')
    lines.append(' */\n')
    lines.append('\n')
    lines.append('#ifndef __WINE_OPENGL32_GUEST_EXT_H\n')
    lines.append('#define __WINE_OPENGL32_GUEST_EXT_H\n')
    lines.append('\n')
    lines.append('#include <wine/wgl.h>\n')
    lines.append('\n')
    n = 0
    for name in registry:
        if name in CALLBACK_REFUSALS:
            continue
        ret, params = pfn[name]
        lines.append('%s GLAPIENTRY %s( %s );\n'
                     % (ret, name, params if params else 'void'))
        n += 1
    lines.append('\n')
    lines.append('/* Deliberately absent, and absent from the guest export surface too:\n')
    for name in sorted(CALLBACK_REFUSALS):
        lines.append(' *   %-26s %s\n' % (name, CALLBACK_REFUSALS[name]))
    lines.append(' * See CALLBACK_REFUSALS in %s. */\n' % GENERATOR)
    lines.append('\n')
    lines.append('#endif /* __WINE_OPENGL32_GUEST_EXT_H */\n')
    return ''.join(lines), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='fail if either output has drifted from its inputs')
    args = ap.parse_args()

    registry = read_registry()
    pfn = read_pfn_typedefs()
    missing = [n for n in registry if n not in pfn]
    if missing:
        die('%d registry entries have no PFN_* typedef in %s: %s'
            % (len(missing), WGL_H, ' '.join(missing[:8])))
    unknown = [n for n in CALLBACK_REFUSALS if n not in registry]
    if unknown:
        die('CALLBACK_REFUSALS names %s, which is not in extension_registry[] '
            '-- a refusal for a name nobody can ask for hides a real one'
            % ' '.join(sorted(unknown)))

    spec, nspec = build_spec(registry, pfn)
    hdr, nhdr = build_header(registry, pfn)
    if nspec != nhdr:
        die('%d spec entries but %d declarations' % (nspec, nhdr))

    rc = 0
    for path, text in ((OUT_SPEC, spec), (OUT_HDR, hdr)):
        old = open(path).read() if os.path.exists(path) else None
        if args.check:
            if old != text:
                print('gen-guest-surface: DRIFT %s is not what its inputs say '
                      'it should be' % os.path.relpath(path, SRC), file=sys.stderr)
                rc = 1
        elif old != text:
            with open(path, 'w') as f:
                f.write(text)
            print('gen-guest-surface: wrote %s' % os.path.relpath(path, SRC))
        else:
            print('gen-guest-surface: %s unchanged' % os.path.relpath(path, SRC))
    if args.check and rc == 0:
        print('gen-guest-surface: CURRENT -- %d registry entry points, '
              '%d refused by name' % (nspec, len(CALLBACK_REFUSALS)))
    elif not args.check:
        print('gen-guest-surface: %d registry entry points, %d refused by name'
              % (nspec, len(CALLBACK_REFUSALS)))
    return rc


if __name__ == '__main__':
    sys.exit(main())
