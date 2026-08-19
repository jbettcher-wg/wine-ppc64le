#!/usr/bin/env python3
"""Generate 32<->64 struct repack functions for any thunked guest surface.

## Why this exists

A guest pointer normally crosses this port's thunk boundary untouched: FEX
shares an address space, and every descriptor struct an x86-64 guest hands us
has the same size, alignment and field offsets as the ppc64le native code
reading it (verified per surface, e.g. dxvk-ppc64le/thunk/gen_layout_check.py).

That guarantee does NOT survive a 32-bit guest.  An i386 image lays pointers
out in four bytes and relaxes alignment to four, so a struct with any pointer
member -- or any member whose alignment was doing work -- moves every field
after it.  Measured on the D3D11/DXGI surface: 47 of 297 aggregates differ.

The wrong answer is to hand-write walkers for the structs some title happens
to pass and refuse the rest.  Wine is not three games; a module that exports a
function must be able to serve it, and a divergence is mechanical -- fields
move, they do not change meaning.  So this generator emits a repack for EVERY
divergent aggregate, in both directions, from measurements rather than from a
reading of the headers.

## How the measurement works, and why it is trustworthy

Two clang invocations per target do all the real work:

  1. `-Xclang -fdump-record-layouts` names each aggregate's fields in order,
     with the offset the target's ABI gives them.  Nested and anonymous
     records are dumped as their own layouts, so nothing is inferred.
  2. a generated translation unit takes `sizeof` of each field, which the
     layout dump does not print.  It is read out of `-S` assembly, never
     executed, so this cross-compiles freely.

A field is then copied by one of three rules, all decided by measurement:

  same size          copy the bytes.
  4 on i386, 8 on 64 a pointer or a pointer-width scalar: zero-extend going
                     up, truncate going down.  The generator REFUSES to guess
                     when the declared type is signed (LONG_PTR, INT_PTR,
                     ptrdiff_t); those get a sign-extending copy instead, and
                     the type name is what decides, since that is the one
                     thing the width measurement cannot tell us.
  anything else      an aggregate: recurse if it is one we know, and refuse
                     by name (loudly, at generation time) if it is not.

Refusals are a generator-time error, not a silent pass-through, because the
failure mode of a silently wrong repack is a corrupted descriptor field --
this project's most expensive class of bug.

## Output

A C header defining, for each divergent type T:

    static void wine_repack32_T( T *dst, const void *src32 );
    static void wine_repack64_T( void *dst32, const T *src );

plus `WINE_REPACK32_SIZE_T` (the i386 size, so a caller can bounds-check the
guest buffer it was handed).  Types that do not diverge get nothing: the
caller passes the guest pointer straight through, as on 64-bit.

    ./gen_repack32.py --headers d3d11_4.h dxgi1_6.h --match 'D3D11_|DXGI_' \\
        --wine-include <build>/include --out <build>/dxvk_repack32.h
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# Fields whose declared type is pointer-width AND SIGNED.  A width measurement
# says only "four bytes there, eight here"; whether the high half should be
# zero or a sign copy is a property of the type, so it is read from the type
# name.  Anything not listed is zero-extended, which is right for pointers and
# for every unsigned pointer-width scalar Windows declares.
SIGNED_PTR_TYPES = frozenset((
    'LONG_PTR', 'INT_PTR', 'SSIZE_T', 'ptrdiff_t', 'intptr_t',
))

LAYOUT_RE = re.compile(r'^\s*\*\*\* Dumping AST Record Layout')
# Depth matters.  clang dumps a nested record's fields INSIDE its parent's
# listing, indented one level further; a pattern that ignores the indent
# attaches LUID's LowPart to DXGI_ADAPTER_DESC3 and then asks for the sizeof
# of a member that does not exist there.  Exactly one space after the bar is
# the record header, exactly three is one of its own fields.
STRUCT_RE = re.compile(r'^\s*0 \| (struct|union) (\S.*?)\s*$')
FIELD_RE = re.compile(r'^\s*(\d+) \|   (\S.*?)\s+(\w+)(\[\d+\])?\s*$')
SIZE_RE = re.compile(r'^\s*\| \[sizeof=(\d+), align=(\d+)\]')


class Aggregate:
    def __init__(self, name):
        self.name = name
        self.fields = []      # (offset, type_str, field_name, array_suffix)
        self.size = None
        self.align = None


def dump_layouts(tu, target, includes, defines):
    """clang's own record layouts for `target` -> {name: Aggregate}."""
    cmd = ['clang', '-target', target, '-nostdlibinc', '-fsyntax-only',
           '-Xclang', '-fdump-record-layouts'] + \
          ['-I' + i for i in includes] + defines + [tu]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[:4000])
        sys.exit('gen_repack32: the layout translation unit did not compile '
                 'for %s (above).  A name harvested from the headers is not '
                 'a type; fix the --match regex rather than dropping it '
                 'silently.' % target)
    out = r.stdout
    aggs, cur = {}, None
    for line in out.splitlines():
        if LAYOUT_RE.match(line):
            cur = None
            continue
        m = STRUCT_RE.match(line)
        if m and cur is None:
            name = m.group(2).strip()
            # anonymous/nested records are dumped under a parenthesised
            # spelling; they are reached through their parent's field list,
            # never by name, so they are not indexable here.
            cur = Aggregate(name if '(' not in name else None)
            if cur.name:
                aggs[cur.name] = cur
            continue
        if cur is not None:
            m = FIELD_RE.match(line)
            if m:
                cur.fields.append((int(m.group(1)), m.group(2).strip(),
                                   m.group(3), m.group(4) or ''))
                continue
            m = SIZE_RE.match(line)
            if m:
                cur.size, cur.align = int(m.group(1)), int(m.group(2))
                cur = None
    return aggs


def field_sizes(names_fields, target, includes, defines, workdir, headers):
    """sizeof() every (type, field) pair, read out of -S assembly."""
    src = os.path.join(workdir, 'fs-%s.c' % target.split('-')[0])
    with open(src, 'w') as fh:
        fh.write(PROLOGUE)
        for h in headers:
            fh.write('#include <%s>\n' % h)
        fh.write('const unsigned int fs[] = {\n')
        for tname, fname in names_fields:
            fh.write('    (unsigned int)sizeof(((%s *)0)->%s),\n'
                     % (tname, fname))
        fh.write('};\n')
    asm = src[:-2] + '.s'
    subprocess.run(['clang', '-target', target, '-nostdlibinc', '-O0', '-S',
                    '-o', asm, src] + ['-I' + i for i in includes] + defines,
                   check=True)
    text = open(asm).read()
    m = re.search(r'^_?fs:\n((?:\s+\.long\s+\d+[^\n]*\n)+)', text, re.M)
    if not m:
        sys.exit('gen_repack32: no field-size array in %s' % asm)
    return [int(x) for x in re.findall(r'\.long\s+(\d+)', m.group(1))]


PROLOGUE = ('#define WIN32_LEAN_AND_MEAN\n#define COBJMACROS\n'
            '#include <windows.h>\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--headers', nargs='+', required=True,
                    help='umbrella headers the generated TU #includes')
    ap.add_argument('--scan', default=r'^(d3d11.*|dxgi.*)\.h$',
                    help='regex over FILE NAMES in the include dir naming '
                         'which headers to harvest aggregate names from.  An '
                         'umbrella header is mostly #includes -- harvesting '
                         'only those found 7 of 297 types -- so the whole '
                         'family is scanned and the umbrella is what the '
                         'generated TU includes.')
    ap.add_argument('--match', default=r'D3D11_|DXGI_',
                    help='regex an aggregate name must match to be considered')
    ap.add_argument('--wine-include', required=True,
                    help='the build tree include dir (widl-generated headers)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--guest64', default='x86_64-windows-gnu')
    ap.add_argument('--guest32', default='i386-windows-gnu')
    args = ap.parse_args()

    includes = [args.wine_include, os.path.join(args.wine_include, 'msvcrt')]
    defines = ['-D_UCRT']
    match = re.compile(args.match)

    with tempfile.TemporaryDirectory() as td:
        # A TU that includes the surface and instantiates every aggregate the
        # headers typedef, so clang lays all of them out.
        scan = re.compile(args.scan)
        names, scanned = set(), []
        for fn in sorted(os.listdir(args.wine_include)):
            if not scan.match(fn):
                continue
            scanned.append(fn)
            text = open(os.path.join(args.wine_include, fn),
                        errors='replace').read()
            for m in re.finditer(r'\}\s*(\w+)\s*;', text):
                if match.search(m.group(1)):
                    names.add(m.group(1))
        names = sorted(names)
        if not names:
            sys.exit('gen_repack32: --scan %r matched no aggregate names'
                     % args.scan)

        # The TU includes every header that was SCANNED, not just the
        # umbrellas: a name harvested from dxgidebug.h is undeclared unless
        # dxgidebug.h is included, and dropping such names silently is the
        # per-title triage this generator exists to avoid.  Include guards
        # make the overlap free.
        includes_h = list(dict.fromkeys(list(args.headers) + scanned))
        includes_out = includes_h
        tu = os.path.join(td, 'tu.c')
        with open(tu, 'w') as fh:
            fh.write(PROLOGUE)
            for h in includes_h:
                fh.write('#include <%s>\n' % h)
            # sizeof, not a tentative definition: -fsyntax-only lays out a
            # record only where the layout is actually needed, and a bare
            # `T x;` is not that -- 127 of 297 came back the first time.
            for i, n in enumerate(names):
                fh.write('const unsigned int force_%d = sizeof(%s);\n' % (i, n))

        a64 = dump_layouts(tu, args.guest64, includes, defines)
        a32 = dump_layouts(tu, args.guest32, includes, defines)

        # Only aggregates whose LAYOUT differs need a repack at all.
        divergent = []
        for n in names:
            x, y = a64.get(n), a32.get(n)
            if not x or not y:
                continue
            if (x.size, x.align) != (y.size, y.align) or \
               [f[0] for f in x.fields] != [f[0] for f in y.fields]:
                divergent.append(n)

        pairs = [(n, f[2]) for n in divergent for f in a64[n].fields]
        if not pairs:
            print('gen_repack32: no divergent aggregates; nothing to emit')
        s64 = field_sizes(pairs, args.guest64, includes, defines, td,
                          includes_h)
        s32 = field_sizes(pairs, args.guest32, includes, defines, td,
                          includes_h)

    idx, sizes = 0, {}
    for n in divergent:
        for f in a64[n].fields:
            sizes[(n, f[2])] = (s64[idx], s32[idx])
            idx += 1

    refused = []
    out = [HEADER]
    # Self-contained: a repack names the types it repacks, so the header
    # carries the includes it was generated from rather than making every
    # consumer reproduce the exact set (dxgidebug.h's queue filters are not
    # reachable from dxgi1_6.h, and a caller that guesses gets an unknown
    # type name for a struct it never mentions).
    out.append('#define WIN32_LEAN_AND_MEAN')
    out.append('#define COBJMACROS')
    out.append('#include <windows.h>')
    for h in includes_out:
        out.append('#include <%s>' % h)
    out.append('')
    # Forward declarations for all of them, so a struct that embeds another
    # divergent struct can call its repack whatever order they are emitted
    # in.  Ordering by nesting depth would work too and is one subtle bug
    # away from an implicit declaration; this cannot be got wrong.
    out.append('/* forward declarations: nested aggregates repack recursively */')
    for n in divergent:
        out.append('static inline void wine_repack32_%s( %s *dst, const void *src32 );'
                   % (n, n))
        out.append('static inline void wine_repack64_%s( void *dst32, const %s *src );'
                   % (n, n))
    out.append('')
    for n in divergent:
        body_up, body_dn = [], []
        for (o64, ty, fname, arr), (o32, _, _, _) in zip(a64[n].fields,
                                                         a32[n].fields):
            w64, w32 = sizes[(n, fname)]
            base = ty.split('[')[0].split()[-1] if ty else ''
            if base in divergent and not arr:
                # a nested aggregate that diverges in its own right: recurse,
                # never memcpy -- its fields have moved too.
                body_up.append('    wine_repack32_%s( (%s *)((char *)dst + %d), (const char *)src32 + %d );'
                               % (base, base, o64, o32))
                body_dn.append('    wine_repack64_%s( (char *)dst32 + %d, (const %s *)((const char *)src + %d) );'
                               % (base, o32, base, o64))
            elif w64 == w32:
                body_up.append('    memcpy( (char *)dst + %d, (const char *)src32 + %d, %d );'
                               % (o64, o32, w64))
                body_dn.append('    memcpy( (char *)dst32 + %d, (const char *)src + %d, %d );'
                               % (o32, o64, w32))
            elif w32 == 4 and w64 == 8:
                base = ty.split()[0]
                signed = base in SIGNED_PTR_TYPES
                ld = ('(ULONG64)(LONG64)*(const INT32 *)' if signed
                      else '(ULONG64)*(const UINT32 *)')
                body_up.append(
                    '    *(ULONG64 *)((char *)dst + %d) = %s((const char *)src32 + %d);'
                    % (o64, ld, o32))
                body_dn.append(
                    '    *(UINT32 *)((char *)dst32 + %d) = (UINT32)*(const ULONG64 *)((const char *)src + %d);'
                    % (o32, o64))
            else:
                refused.append('%s.%s (%s: %d bytes on the 64-bit guest, %d on '
                               'the 32-bit one -- not a pointer widening)'
                               % (n, fname, ty, w64, w32))
        out.append('/* %s: %d bytes / align %d on a 64-bit guest, %d / %d on i386 */'
                   % (n, a64[n].size, a64[n].align, a32[n].size, a32[n].align))
        out.append('#define WINE_REPACK32_SIZE_%s %d' % (n, a32[n].size))
        out.append('static inline void wine_repack32_%s( %s *dst, const void *src32 )\n{'
                   % (n, n))
        out.extend(body_up)
        out.append('}')
        out.append('static inline void wine_repack64_%s( void *dst32, const %s *src )\n{'
                   % (n, n))
        out.extend(body_dn)
        out.append('}\n')

    if refused:
        sys.stderr.write('gen_repack32: REFUSING to guess %d field(s):\n  %s\n'
                         % (len(refused), '\n  '.join(refused)))
        return 1

    with open(args.out, 'w') as fh:
        fh.write('\n'.join(out) + '\n')
    print('gen_repack32: %d of %d aggregates diverge and were repacked -> %s'
          % (len(divergent), len(names), args.out))
    return 0


HEADER = '''/* GENERATED by ppc64le/thunks/gen_repack32.py -- do not edit.
 *
 * 32<->64 struct repacks for the guest thunk surface.  A 32-bit guest lays
 * pointer members out in four bytes and aligns to four, so every aggregate
 * listed here has fields at different offsets than the native code reading
 * it expects.  Each function copies field by field at MEASURED offsets --
 * clang's own record layouts for both targets, plus a sizeof pass -- so a
 * header change moves the offsets here too rather than silently corrupting
 * a descriptor.
 *
 * repack32_X( dst, src32 )  guest 32-bit struct  -> native layout   (IN)
 * repack64_X( dst32, src )  native layout -> guest 32-bit struct    (OUT)
 *
 * Aggregates that do NOT diverge appear nowhere in this file: their guest
 * pointer is passed through untouched, exactly as on the 64-bit lane.
 */

#pragma once

#include <string.h>
'''


if __name__ == '__main__':
    sys.exit(main())
