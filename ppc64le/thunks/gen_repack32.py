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

# Pointer-width INTEGER types: 4 guest bytes hold a VALUE, not an address.
# Narrowing one on the way OUT is what 32-bit Windows itself does -- with a
# CLAMP, not a truncation: DXGI_ADAPTER_DESC's DedicatedVideoMemory on a
# WoW64 process saturates at 4 GiB rather than reporting memory mod 2^32,
# and a truncated size is the wrong-number class this tree refuses to ship.
UNSIGNED_PTR_TYPES = frozenset((
    'SIZE_T', 'ULONG_PTR', 'UINT_PTR', 'uintptr_t', 'DWORD_PTR',
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


TYPEDEF_OPEN_RE = re.compile(r'\btypedef\s+(struct|union|enum)\s+(\w+)?\s*\{')


def harvest_typedefs(text):
    """-> [(kind, tag, typedef_name)] for every `typedef struct TAG {...} N;`.

    Why this exists: clang's record dump names a record by its TAG, and the
    two spellings are only the same by convention.  The d3d11/dxgi headers
    happen to write `typedef struct D3D11_FOO { ... } D3D11_FOO;` -- tag and
    typedef identical -- so indexing the dump by tag and looking it up by
    typedef name worked there by luck.  d3d9.h's dialect does not: its tags
    are `_D3DPRESENT_PARAMETERS_`, `_D3DCAPS9`, `_D3DLOCKED_RECT`.  Every one
    of those lookups missed, `measured` silently dropped the whole surface,
    and the generator reported "0 of 94 aggregates diverge" -- a clean-looking
    run over nothing at all.  D3DPRESENT_PARAMETERS carries an HWND and DOES
    diverge, so that clean run was a lie of exactly the kind this file's
    docstring promises not to tell.  [MEASURED 2026-08-30]

    Brace-matched rather than regexed closed, because D3DMATRIX nests an
    anonymous union inside an anonymous struct and no fixed-depth pattern
    survives that.
    """
    out = []
    for m in TYPEDEF_OPEN_RE.finditer(text):
        depth, i, n = 1, m.end(), len(text)
        while i < n and depth:
            c = text[i]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            i += 1
        tail = re.match(r'\s*(\w+)\s*[,;]', text[i:])
        if tail:
            out.append((m.group(1), m.group(2), tail.group(1)))
    return out


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
    ap.add_argument('--exclude', default=None,
                    help='regex over aggregate NAMES to drop from the harvest '
                         'after --match.  The harvest is a text scan and does '
                         'not run the preprocessor, so a typedef sitting '
                         'inside an #if 0 block is harvested and then fails '
                         'to compile -- d3d9types.h has exactly one, '
                         'D3DORDERTYPE, dead since D3D8.  Every exclusion has '
                         'to be spelled out on the command line, in the tree, '
                         'with a reason; the generator still refuses to drop '
                         'anything on its own.')
    ap.add_argument('--wine-include', required=True,
                    help='the build tree include dir (widl-generated headers)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--json', help='also write the audited layout roster '
                    '(every aggregate measured, divergent or not, with both '
                    'sizes) for gen_winecom.py to key its per-slot i386 '
                    'struct audit on')
    ap.add_argument('--guest64', default='x86_64-windows-gnu')
    ap.add_argument('--guest32', default='i386-windows-gnu')
    args = ap.parse_args()

    includes = [args.wine_include, os.path.join(args.wine_include, 'msvcrt')]
    defines = ['-D_UCRT']
    match = re.compile(args.match)
    exclude = re.compile(args.exclude) if args.exclude else None

    with tempfile.TemporaryDirectory() as td:
        # A TU that includes the surface and instantiates every aggregate the
        # headers typedef, so clang lays all of them out.
        scan = re.compile(args.scan)
        names, scanned = set(), []
        record_kind = {}   # name -> 'struct'/'union'; enums are not records
        tag_of = {}        # name -> the struct TAG clang dumps it under
        for fn in sorted(os.listdir(args.wine_include)):
            if not scan.match(fn):
                continue
            scanned.append(fn)
            text = open(os.path.join(args.wine_include, fn),
                        errors='replace').read()
            # `} NAME;` and `} NAME, *LPNAME;` alike -- d3d9types.h writes the
            # second form for 20-odd types including D3DVERTEXELEMENT9, and a
            # pattern anchored on the semicolon harvests none of them.
            for m in re.finditer(r'\}\s*(\w+)\s*[,;]', text):
                if match.search(m.group(1)):
                    if exclude and exclude.search(m.group(1)):
                        continue
                    names.add(m.group(1))
            for kind, tag, name in harvest_typedefs(text):
                if not match.search(name):
                    continue
                if exclude and exclude.search(name):
                    continue
                if kind in ('struct', 'union'):
                    record_kind[name] = kind
                    if tag:
                        tag_of[name] = tag
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

        # clang indexed those by struct TAG.  Publish each record under its
        # TYPEDEF name as well, which is the only spelling a signature ever
        # uses.  See harvest_typedefs' docstring for what the missing alias
        # cost on the d3d9 surface.
        for aggs in (a64, a32):
            for nm, tg in tag_of.items():
                if nm not in aggs and tg in aggs:
                    aggs[nm] = aggs[tg]

        # Every measured aggregate's field widths, on both targets.  The
        # sizes are needed for MORE than the repack bodies: the divergence
        # test itself must see them, because a pointer member can land at
        # the SAME offset in a struct of the SAME size ({void *p; UINT64 b}
        # is 16 bytes with b at 8 on both guests) while the pointer itself
        # is 4 guest bytes -- the native read of 8 picks up padding.  The
        # first version of this test compared only sizes, aligns and
        # offsets, and would have passed that struct through raw.
        measured = [n for n in names if a64.get(n) and a32.get(n)
                    and [f[2] for f in a64[n].fields] ==
                        [f[2] for f in a32[n].fields]]
        # A harvested name that the headers declare as a RECORD and that came
        # back unmeasured is a hole in the audit, not a detail: gen_winecom
        # keys its per-slot i386 struct check on this roster, and a type
        # absent from both lists is one it will refuse -- or, worse, one a
        # caller assumes was checked.  Enums are legitimately unmeasurable
        # and are not counted.  Fail loudly rather than emit a short roster.
        lost = sorted(n for n in names
                      if n in record_kind and n not in measured)
        if lost:
            sys.exit('gen_repack32: %d harvested RECORD(s) were never '
                     'measured on both targets -- the roster would be '
                     'silently short.  Usually a tag/typedef spelling this '
                     'file does not alias yet:\n  %s'
                     % (len(lost), '\n  '.join(lost)))
        pairs = [(n, f[2]) for n in measured for f in a64[n].fields]
        if not pairs:
            print('gen_repack32: no measurable aggregates; nothing to emit')
        s64 = field_sizes(pairs, args.guest64, includes, defines, td,
                          includes_h)
        s32 = field_sizes(pairs, args.guest32, includes, defines, td,
                          includes_h)

    idx, sizes = 0, {}
    for n in measured:
        for f in a64[n].fields:
            sizes[(n, f[2])] = (s64[idx], s32[idx])
            idx += 1

    divergent = []
    for n in measured:
        x, y = a64[n], a32[n]
        if (x.size, x.align) != (y.size, y.align) or \
           [f[0] for f in x.fields] != [f[0] for f in y.fields] or \
           any(sizes[(n, f[2])][0] != sizes[(n, f[2])][1]
               for f in x.fields):
            divergent.append(n)

    refused = []
    out = [HEADER]
    # Self-contained: a repack names the types it repacks, so the header
    # carries the includes it was generated from rather than making every
    # consumer reproduce the exact set (dxgidebug.h's queue filters are not
    # reachable from dxgi1_6.h, and a caller that guesses gets an unknown
    # type name for a struct it never mentions).
    # NO type names anywhere in the output: every body is offset-based byte
    # copying, so the untyped void* signatures free the consumer from
    # reproducing the exact DXVK header set this was generated against --
    # which is what lets dlls/d3d11/main.c, which deliberately includes no
    # D3D headers at all, include this file and hand the functions to
    # libs/winecom's rep tables.
    # Forward declarations for all of them, so a struct that embeds another
    # divergent struct can call its repack whatever order they are emitted
    # in.  Ordering by nesting depth would work too and is one subtle bug
    # away from an implicit declaration; this cannot be got wrong.
    out.append('/* forward declarations: nested aggregates repack recursively */')
    for n in divergent:
        out.append('static inline void wine_repack32_%s( void *dst, const void *src32 );'
                   % n)
        out.append('static inline void wine_repack64_%s( void *dst32, const void *src );'
                   % n)
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
                body_up.append('    wine_repack32_%s( (char *)dst + %d, (const char *)src32 + %d );'
                               % (base, o64, o32))
                body_dn.append('    wine_repack64_%s( (char *)dst32 + %d, (const char *)src + %d );'
                               % (base, o32, o64))
            elif w64 == w32:
                body_up.append('    memcpy( (char *)dst + %d, (const char *)src32 + %d, %d );'
                               % (o64, o32, w64))
                body_dn.append('    memcpy( (char *)dst32 + %d, (const char *)src + %d, %d );'
                               % (o32, o64, w32))
            elif w32 == 4 and w64 == 8:
                base = ty.split()[0]
                signed = base in SIGNED_PTR_TYPES
                value_kind = signed or base in UNSIGNED_PTR_TYPES
                ld = ('(unsigned long long)(long long)*(const int *)' if signed
                      else '(unsigned long long)*(const unsigned int *)')
                body_up.append(
                    '    *(unsigned long long *)((char *)dst + %d) = %s((const char *)src32 + %d);'
                    % (o64, ld, o32))
                if value_kind and not signed:
                    # a pointer-width COUNT going down to 4 bytes SATURATES,
                    # exactly as WoW64's own adapter-memory fields do
                    body_dn.append(
                        '    { unsigned long long v = *(const unsigned long long *)((const char *)src + %d);'
                        ' *(unsigned int *)((char *)dst32 + %d) ='
                        ' v > 0xffffffffu ? 0xffffffffu : (unsigned int)v; }'
                        % (o64, o32))
                elif signed:
                    body_dn.append(
                        '    { long long v = *(const long long *)((const char *)src + %d);'
                        ' *(int *)((char *)dst32 + %d) ='
                        ' v > 0x7fffffff ? 0x7fffffff : v < (-0x7fffffff - 1) ? (-0x7fffffff - 1) : (int)v; }'
                        % (o64, o32))
                else:
                    body_dn.append(
                        '    *(unsigned int *)((char *)dst32 + %d) = (unsigned int)*(const unsigned long long *)((const char *)src + %d);'
                        % (o32, o64))
            else:
                refused.append('%s.%s (%s: %d bytes on the 64-bit guest, %d on '
                               'the 32-bit one -- not a pointer widening)'
                               % (n, fname, ty, w64, w32))
        out.append('/* %s: %d bytes / align %d on a 64-bit guest, %d / %d on i386 */'
                   % (n, a64[n].size, a64[n].align, a32[n].size, a32[n].align))
        out.append('#define WINE_REPACK32_SIZE_%s %d' % (n, a32[n].size))
        out.append('static inline void wine_repack32_%s( void *dst, const void *src32 )\n{'
                   % n)
        out.extend(body_up)
        out.append('}')
        out.append('static inline void wine_repack64_%s( void *dst32, const void *src )\n{'
                   % n)
        out.extend(body_dn)
        out.append('}\n')

    if refused:
        sys.stderr.write('gen_repack32: REFUSING to guess %d field(s):\n  %s\n'
                         % (len(refused), '\n  '.join(refused)))
        return 1

    with open(args.out, 'w') as fh:
        fh.write('\n'.join(out) + '\n')
    if args.json:
        import json as _json
        # A widened field is a 4-byte guest cell that native code reads or
        # writes as 8.  Widening IN (guest -> native) is always safe.  The
        # OUT direction truncates, which is safe only for the window-station
        # handle family -- Wine keeps those below 4 GiB by design -- and a
        # SILENT truncation of a real pointer (Map's pData) is this
        # codebase's most expensive bug class, so any other widened field
        # marks the type out_unsafe and the consumer must refuse the out
        # direction rather than repack it.
        handle_ok = re.compile(r'^H[A-Z]{2,}$')
        def out_unsafe(n):
            for o64, ty, fname, arr in a64[n].fields:
                w64, w32 = sizes[(n, fname)]
                if w64 == 8 and w32 == 4:
                    base = (ty or '').split('[')[0].strip()
                    last = base.split()[-1] if base.split() else ''
                    if last in SIGNED_PTR_TYPES or last in UNSIGNED_PTR_TYPES:
                        continue     # a value, clamped by the repack, not an address
                    if '*' in base or not handle_ok.match(last):
                        return True
            return False
        roster = dict(
            divergent={n: dict(size64=a64[n].size, size32=a32[n].size,
                               out_unsafe=out_unsafe(n))
                       for n in divergent},
            identical=sorted(n for n in measured if n not in divergent))
        with open(args.json, 'w') as fh:
            _json.dump(roster, fh, indent=1, sort_keys=True)
            fh.write('\n')
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

/* ------------------------------------------- pointer-width SCALAR pointees
 *
 * Not an aggregate, and emitted unconditionally because it is a property of
 * the two ABIs rather than of any header: a parameter declared `HANDLE *h`
 * (or SIZE_T *, ULONG_PTR *, HMODULE * ...) points at a cell that is FOUR
 * bytes in the guest and EIGHT in the native callee.  Passing that guest
 * pointer through raw makes the callee read eight bytes of a four-byte cell
 * and, worse, write eight over it -- four bytes of the guest's own stack
 * frame or heap block, silently.
 *
 * D3D9 is where this stopped being theoretical: `HANDLE *pSharedHandle` is
 * the last parameter of EVERY resource creator -- CreateTexture,
 * CreateVertexBuffer, CreateIndexBuffer, CreateRenderTarget,
 * CreateDepthStencilSurface, CreateCubeTexture, CreateVolumeTexture,
 * CreateOffscreenPlainSurface and the three *Ex twins -- nineteen rows, i.e.
 * every way a D3D9 title can allocate anything.  [MEASURED 2026-08-30]
 *
 * THE NARROWING IS THE HONEST PART.  Going up (IN) is a zero-extend and
 * cannot lose.  Coming down (OUT) can: a handle whose high half is set does
 * not fit the guest's cell.  It is NOT clamped -- a clamped handle names a
 * different object, which is worse than none -- so the loss is reported and
 * the cell is zeroed, which is what "no shared handle" means to every caller
 * of these methods.  In practice DXVK's d3d9 answers pSharedHandle only for
 * shared resources and NT handles are 32-bit values, so this is a tripwire
 * rather than a path anything is expected to take.
 */
#ifndef WINE_REPACK32_PTRWIDTH_DEFINED
#define WINE_REPACK32_PTRWIDTH_DEFINED
/* Defined in libs/winecom, which every consumer of this header already
 * imports.  Declared here rather than included from a Wine header, because
 * this file deliberately pulls in nothing but <string.h>. */
extern void wine_repack32_ptrwidth_lost( unsigned long long v );

static inline void wine_repack32_PTRWIDTH( void *dst, const void *src32 )
{
    unsigned int lo;
    memcpy( &lo, src32, 4 );
    *(unsigned long long *)dst = (unsigned long long)lo;
}
static inline void wine_repack64_PTRWIDTH( void *dst32, const void *src )
{
    unsigned long long v = *(const unsigned long long *)src;
    unsigned int lo = (unsigned int)v;

    if (v >> 32)
    {
        static int logged;
        if (!logged)
        {
            logged = 1;
            wine_repack32_ptrwidth_lost( v );
        }
        lo = 0;
    }
    memcpy( dst32, &lo, 4 );
}
#endif
'''


if __name__ == '__main__':
    sys.exit(main())
