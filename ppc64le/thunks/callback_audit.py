#!/usr/bin/env python3
"""
callback_audit.py -- every native export that takes a guest FUNCTION POINTER,
checked against the rows that wrap one.

WHY THIS EXISTS
---------------
A guest hands a native module a pointer to its own code -- a WNDPROC, a
DLGPROC, a qsort comparator, a WINHTTP_STATUS_CALLBACK -- and the native
module stores it and calls it later.  There is nothing in the pointer that
says which machine it belongs to, so unless the port swaps it for a
trampoline at the moment of registration, the native ppc64 core eventually
executes x86-64 bytes.

That failure does not announce itself.  The core decodes the guest's bytes as
ppc64 instructions and runs them; the first one that touches memory raises an
access violation at an address INSIDE the guest image, which reads exactly
like the game dereferencing a null pointer.  DOOM (2016) cost this port days
in that shape twice -- once through gdi32's font enumeration, and once through
user32's DialogBoxParamA and winhttp's WinHttpSetStatusCallback, which is what
prompted this file.

The wrapping rows live in `thunk_overrides[]` in dlls/ntdll/signal_ppc64.c.
This audit reads Wine's own headers through the same clang oracle the thunk
generator uses, finds every exported function with a function-pointer
parameter, and asks whether a row covers that argument.  What is left over is
the exact list of holes -- and `holes.txt` beside this script is that list,
matched EXACTLY, so it cannot rot in either direction: a new hole fails the
gate, and a hole that gets filled without being struck off fails it too.
"""

import argparse, glob, json, os, re, subprocess, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'tools', 'spec2thunk'))
import wine_sig


# --------------------------------------------------------------- the rows

ROW_RE = re.compile(
    r'\{\s*L"(?P<mod>[^"]+)"\s*,\s*"(?P<name>[^"]+)"\s*,\s*(?P<argc>\d+)\s*,'
    r'\s*(?P<func>[A-Za-z_0-9]+)\s*(?:,\s*(?P<cb>[^,}]+))?'
    r'(?:,\s*(?P<wide>[^,}]+))?(?:,\s*(?P<cbargc>[^,}]+))?\s*\}')


def parse_mask(text):
    """`1u << 2 | 1u << 3` -> 0b1100.  Anything unparsable raises."""
    if text is None:
        return 0
    text = text.strip()
    if not text or text == '0':
        return 0
    total = 0
    for term in text.split('|'):
        m = re.fullmatch(r'\s*1u?\s*<<\s*(\d+)\s*', term)
        if not m:
            raise ValueError('cannot read a callback mask from %r' % text)
        total |= 1 << int(m.group(1))
    return total


def read_rows(signal_c):
    """-> {(module.lower(), name): (cb_mask, has_override)}"""
    with open(signal_c) as f:
        text = f.read()
    start = text.index('static const struct thunk_override thunk_overrides[]')
    end = text.index('\n};', start)
    rows = {}
    for m in ROW_RE.finditer(text[start:end]):
        key = (m.group('mod').lower(), m.group('name'))
        rows[key] = (parse_mask(m.group('cb')), m.group('func') != 'NULL')
    if not rows:
        raise SystemExit('FATAL: no thunk_overrides rows parsed from %s' % signal_c)
    return rows


# ------------------------------------------------------- the thunk surface

SPEC_RE = re.compile(r'^\s*(?:@|\d+)\s+(?:stdcall|cdecl|varargs)\s+'
                     r'(?:-\S+\s+)*(?P<name>[A-Za-z_][A-Za-z_0-9@?$]*)\s*\(')


def thunked_modules(source):
    """-> [(module_dll_name, spec_path)] for every module with a .thunks file."""
    out = []
    for thunks in sorted(glob.glob(os.path.join(source, 'dlls', '*', '*.thunks'))):
        dll = None
        with open(thunks) as f:
            for line in f:
                if line.startswith('DLL '):
                    dll = line.split(None, 1)[1].strip()
                    break
        if not dll:
            continue
        d = os.path.dirname(thunks)
        specs = glob.glob(os.path.join(d, '*.spec'))
        if len(specs) != 1:
            continue
        out.append((dll, specs[0]))
    return out


def spec_exports(path):
    names = []
    with open(path) as f:
        for line in f:
            line = line.split('#', 1)[0]
            m = SPEC_RE.match(line)
            if m:
                names.append(m.group('name'))
    return names


# ------------------------------------------------ is a type a code pointer?

FUNCPTR_RE = re.compile(r'\(\s*[^()]*\*\s*\)\s*\(')


def function_pointer_types(headers, spellings):
    """-> the subset of `spellings` that are pointers to functions.

    MEASURED BY CLANG, not by the look of the name.  A typedef chain is
    followed for us: clang's JSON AST prints `desugaredQualType` fully
    desugared, so `DLGPROC` comes back as `INT_PTR (*)(HWND, UINT, WPARAM,
    LPARAM)` however many typedefs deep it was.  A spelling that does not
    compile as a type simply does not appear in the answer, and the caller
    reports it as unknown rather than as clean.
    """
    spellings = sorted(spellings)
    src = [headers.probe_src]
    for n, sp in enumerate(spellings):
        src.append('typedef %s wt_cbty_%d;\n' % (sp, n))
    path = os.path.join(headers.workdir, 'wt_cbty.c')
    with open(path, 'w') as f:
        f.write(''.join(src))
    cmd = headers._base() + ['-Xclang', '-ast-dump=json', path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not r.stdout:
        raise SystemExit('FATAL: clang produced no AST for the type probe:\n%s'
                         % r.stderr[:2000])
    found = {}
    for chunk in wine_sig.WineHeaders._split_json(r.stdout):
        stack = [chunk]
        while stack:
            node = stack.pop()
            if isinstance(node, dict):
                name = node.get('name', '')
                if (node.get('kind') == 'TypedefDecl'
                        and isinstance(name, str) and name.startswith('wt_cbty_')):
                    ty = node.get('type', {})
                    spelled = ty.get('desugaredQualType') or ty.get('qualType', '')
                    found[int(name.rsplit('_', 1)[1])] = spelled
                stack.extend(node.get('inner', []) or [])
            elif isinstance(node, list):
                stack.extend(node)
    resolved = {spellings[i]: s for i, s in found.items()}
    missing = [sp for i, sp in enumerate(spellings) if i not in found]
    return ({sp for sp, s in resolved.items() if FUNCPTR_RE.search(s)},
            resolved, missing)



# ------------------------------------------------------- candidate rows

DECL_RE = re.compile(r'^(?P<ret>.*?)\(\s*[^()]*\*+\s*\)\s*\((?P<args>.*)\)\s*$')


def emit_rows( headers, findings, resolved, surface ):
    """Print a candidate row per hole.

    The two numbers a row cannot be written without -- how many arguments the
    CALLBACK takes and whether its return is a full 64 bits -- are taken from
    the callback's own canonical type: the arity by parsing the parameter list
    at depth zero (wine_sig._split_top, the same splitter the generator uses),
    the width by MEASURING sizeof of the return type with the same clang.  A
    callback whose type will not decompose, or whose return will not compile,
    gets no row and is printed as such rather than guessed at.

    These are candidates.  A row is wrong wherever the pointer is not a
    registration at all -- RtlUserThreadStart takes the thread's entry point
    and is called BY this port, not by a guest -- so read each one.
    """
    arity = {(dll, name): len(params) for dll, name, params in surface}
    wanted, decomposed = [], {}
    for dll, name, idx, types in findings:
        for t in types:
            if t in decomposed: continue
            m = DECL_RE.match( resolved.get(t, '') )
            if not m: continue
            ret = m.group('ret').strip()
            params = [p for p in wine_sig._split_top( m.group('args') ) if p and p != 'void']
            decomposed[t] = (ret, len(params))
            wanted.append( (t, [ret]) )
    widths = headers.widths( wanted )

    for dll, name, idx, types in findings:
        argc = arity.get( (dll, name), 0 )
        for i, t in zip( idx, types ):
            if t not in decomposed:
                print( '/* %s %s arg%d: %s -- type does not decompose, no row */'
                       % (dll, name, i, t) )
                continue
            ret, cb_argc = decomposed[t]
            size = (widths.get( t ) or [None])[0]
            if cb_argc > 9:
                # the pool has fixed-arity dispatchers for 4 through 9 only
                print( '/* %s %s arg%d: %s takes %d arguments; the trampoline pool '
                       'tops out at 9, so this needs the pool extended before it '
                       'can have a row */' % (dll, name, i, t, cb_argc) )
                continue
            if ret == 'void':
                wide = '0'
            elif size is None:
                print( '/* %s %s arg%d: return %s not measurable, no row */'
                       % (dll, name, i, ret) )
                continue
            else:
                wide = ('1u << %d' % i) if size == 8 else '0'
            print( '    { L"%s", %-34s %2d, NULL, 1u << %d, %-9s %d },   /* %s: %s */'
                   % (dll, '"%s",' % name, argc, i, wide + ',',
                      cb_argc if cb_argc > 4 else 0, t, resolved[t]) )


# ------------------------------------------------------------------- main

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', default=os.path.join(here, '..', '..'),
                    help='top of the wine source tree')
    ap.add_argument('--build', required=True,
                    help='build directory (for the widl-generated headers)')
    ap.add_argument('--clang', default='clang')
    ap.add_argument('--signal-c',
                    help='read thunk_overrides[] from this file instead of the '
                         'tree\'s dlls/ntdll/signal_ppc64.c -- what the gate\'s '
                         'negative control points at a copy with a row removed')
    ap.add_argument('--holes', default=os.path.join(here, 'callback_holes.txt'),
                    help='the documented, exactly-matched list of known holes')
    ap.add_argument('--update-holes', action='store_true',
                    help='rewrite the holes file from this run (review the diff)')
    ap.add_argument('--module', action='append',
                    help='audit only these modules (repeatable)')
    ap.add_argument('--emit-rows', action='store_true',
                    help='print a candidate thunk_overrides[] row per hole, with '
                         'the callback\'s own arity and return width MEASURED. '
                         'Candidates, not answers: read the section in the header '
                         'about the ones that must not be wrapped.')
    args = ap.parse_args()

    source = os.path.abspath(args.source)
    generated = os.path.join(os.path.abspath(args.build), 'include')
    if not os.path.isfile(os.path.join(generated, 'wtypes.h')):
        raise SystemExit('FATAL: %s does not hold the widl-generated headers; '
                         'pass --build <builddir>' % generated)

    headers = wine_sig.WineHeaders(os.path.join(source, 'include'), generated,
                                   clang=args.clang,
                                   workdir=os.path.join(args.build, 'cbaudit'))
    headers.sanity()

    rows = read_rows(args.signal_c or
                     os.path.join(source, 'dlls', 'ntdll', 'signal_ppc64.c'))

    modules = thunked_modules(source)
    if args.module:
        want = {m.lower() for m in args.module}
        modules = [m for m in modules if m[0].lower() in want]
    if not modules:
        raise SystemExit('FATAL: no thunked modules found under %s/dlls' % source)

    # One AST pass over the whole header surface, then a lookup per export.
    surface = []          # (module, name, [param spellings])
    for dll, spec in modules:
        for name in spec_exports(spec):
            try:
                decl, ret, params = headers._shape(name, allow_variadic=True)
            except Exception:
                continue
            if params:
                surface.append((dll, name, params))

    spellings = {p for _, _, ps in surface for p in ps}
    fps, resolved, missing = function_pointer_types(headers, spellings)

    findings = []
    for dll, name, params in surface:
        idx = [i for i, p in enumerate(params) if p in fps]
        if not idx:
            continue
        mask, has_override = rows.get((dll.lower(), name), (0, False))
        uncovered = [i for i in idx if not (mask >> i) & 1]
        if uncovered and not has_override:
            findings.append((dll.lower(), name, uncovered,
                             [params[i] for i in uncovered]))
    findings.sort()

    if args.emit_rows:
        emit_rows( headers, findings, resolved, surface )
        return 0

    lines = ['%s %s arg%s %s' % (dll, name, ','.join(str(i) for i in idx),
                                 ' '.join(t.replace(' ', '') for t in types))
             for dll, name, idx, types in findings]

    print('callback rows: %d; exports audited: %d; type spellings measured: %d'
          % (len(rows), len(surface), len(spellings)))
    if missing:
        print('type spellings clang could not resolve (reported, not assumed '
              'clean): %d' % len(missing))
        for sp in missing[:20]:
            print('  ?  %s' % sp)
    print('exports taking a guest function pointer with no row: %d' % len(lines))

    if args.update_holes:
        with open(args.holes, 'w') as f:
            f.write('# Exports that take a guest function pointer and have no\n'
                    '# wrapping row in thunk_overrides[].  Written by\n'
                    '# callback_audit.py --update-holes; matched EXACTLY by the\n'
                    '# gate, so filling one means striking it off here.\n')
            f.write('\n'.join(lines) + '\n')
        print('wrote %s (%d entries)' % (args.holes, len(lines)))
        return 0

    known = []
    if os.path.isfile(args.holes):
        with open(args.holes) as f:
            known = [l.strip() for l in f if l.strip() and not l.startswith('#')]
    new = [l for l in lines if l not in known]
    gone = [l for l in known if l not in lines]
    for l in new:
        print('  NEW HOLE   %s' % l)
    for l in gone:
        print('  FILLED     %s  (strike it from %s)' % (l, os.path.basename(args.holes)))
    if new or gone:
        print('FAIL: the callback surface and %s disagree'
              % os.path.basename(args.holes))
        return 1
    print('PASS: every export taking a guest function pointer is either wrapped '
          'by a row or listed as a known hole')
    return 0


if __name__ == '__main__':
    sys.exit(main())
