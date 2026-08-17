#!/usr/bin/env python3
"""
import_chain.py -- resolve a third-party PE's whole static import chain against
this tree's AMD64 guest thunk surface, and name every import that would not
bind.

WHY THIS EXISTS
---------------
A native x86-64 DLL that an application ships or loads -- Microsoft's own
mfc140u.dll, Razer's CChromaEditorLibrary.dll, a game's engine library -- is a
GUEST image on this port, so every one of its static imports has to be served
by a module of its OWN machine.  Those are the x86_64-windows thunk PEs
tools/spec2thunk builds.  Where an export is missing the guest loader does not
fail the load: it binds ntdll's per-symbol sentinel and the process dies at the
first CALL, far from the cause, as 0xDEAD00nn in a module with no symbols.

That is what happened to DOOM (2016): CChromaEditorLibrary.dll needed
mfc140u.dll, mfc140u's DllMain called VCRUNTIME140.__vcrt_InitializeCriticalSectionEx,
and the answer was a sentinel.  The whole chain is decidable BEFORE running
anything -- an import table and an export table are both just tables -- so it
should never again take a game to find one.

WHAT IT CHECKS
--------------
For each subject PE, recursively:

  * every imported DLL name resolves to something that can serve it -- an
    apiset (dlls/apisetschema/apisetschema.spec, the same file Wine builds its
    map from) redirected to its target, then a built guest thunk PE, or another
    real PE sitting beside the subject (which is then walked too);
  * every imported SYMBOL is exported by that module, BY NAME or BY ORDINAL,
    exactly as the guest loader will look it up.

Anything that fails is a HOLE and is named with the module that wanted it.

Exit 0 with a report on stdout; the caller decides what is fatal.  Output is
one TSV record per unbound import:

    HOLE\t<importer>\t<target-module>\t<name-or-#ordinal>

plus a SUMMARY line.  Everything else goes to stderr.
"""

import os
import re
import struct
import sys


# --------------------------------------------------------------------------
# a minimal PE reader -- imports and exports, nothing else
# --------------------------------------------------------------------------

class PE:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b'MZ':
            raise ValueError('%s: not a PE (no MZ)' % path)
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('%s: not a PE (no PE\\0\\0)' % path)
        self.machine = struct.unpack_from('<H', d, pe + 4)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        magic = struct.unpack_from('<H', d, pe + 24)[0]
        if magic != 0x20b:
            raise ValueError('%s: not PE32+ (magic %#x)' % (path, magic))
        self.dd = pe + 24 + 112
        self.secs = []
        so = pe + 24 + optsz
        for i in range(nsec):
            o = so + 40 * i
            vsz, va, rsz, ro = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((va, vsz, ro, rsz))

    def off(self, rva):
        """File offset for an RVA, or None when it lands in uninitialised
        space (a BSS tail has no bytes on disk to read)."""
        for va, vsz, ro, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                o = rva - va
                return ro + o if o < rsz else None
        return None

    def cstr(self, rva):
        o = self.off(rva)
        if o is None:
            return None
        e = self.d.find(b'\0', o)
        return self.d[o:e].decode('latin-1')

    def datadir(self, i):
        return struct.unpack_from('<II', self.d, self.dd + 8 * i)

    def imports(self):
        """-> [(dllname, [name or ('#', ordinal), ...]), ...]"""
        rva, size = self.datadir(1)
        if not rva:
            return []
        out = []
        o = self.off(rva)
        if o is None:
            return []
        while True:
            olt, _ts, _fc, nrva, fthunk = struct.unpack_from('<IIIII', self.d, o)
            if not (olt or nrva or fthunk):
                break
            dll = self.cstr(nrva) or '<unnamed>'
            syms = []
            t = self.off(olt or fthunk)
            if t is not None:
                k = 0
                while True:
                    e = struct.unpack_from('<Q', self.d, t + 8 * k)[0]
                    if e == 0:
                        break
                    if e & (1 << 63):
                        syms.append(('#', e & 0xffff))
                    else:
                        # IMAGE_IMPORT_BY_NAME: WORD hint, then the name
                        syms.append(self.cstr((e & 0x7fffffff) + 2))
                    k += 1
            out.append((dll, syms))
            o += 20
        return out

    def exports(self):
        """-> (set of exported names, set of exported ordinals)

        Only entries with a non-zero address-table slot count.  A zero slot is
        exactly what spec2thunk leaves for a refused export, and it is what the
        guest loader reads as "no implementation" -- so an ordinal HOLE must
        not be reported here as served."""
        rva, size = self.datadir(0)
        names, ords = set(), set()
        if not rva:
            return names, ords
        o = self.off(rva)
        if o is None:
            return names, ords
        (_flags, _ts, _mj, _mn, _nrva, base, naddr, nnames,
         addr_rva, names_rva, ord_rva) = struct.unpack_from('<IIHHIIIIIII', self.d, o)
        ao = self.off(addr_rva)
        live = []
        for i in range(naddr):
            fn = struct.unpack_from('<I', self.d, ao + 4 * i)[0] if ao is not None else 0
            live.append(fn != 0)
            if fn:
                ords.add(base + i)
        no = self.off(names_rva)
        oo = self.off(ord_rva)
        if no is not None and oo is not None:
            for i in range(nnames):
                nm = self.cstr(struct.unpack_from('<I', self.d, no + 4 * i)[0])
                idx = struct.unpack_from('<H', self.d, oo + 2 * i)[0]
                if nm is not None and idx < len(live) and live[idx]:
                    names.add(nm)
        return names, ords


# --------------------------------------------------------------------------
# resolution: apiset -> module -> built guest thunk PE
# --------------------------------------------------------------------------

def read_apisets(spec_path):
    """dlls/apisetschema/apisetschema.spec -> {apiset-name: target-dll}.

    The .spec writes the contract name WITHOUT the extension
    (`apiset api-ms-win-crt-heap-l1-1-0 = ucrtbase.dll`) while an import table
    always carries one, so both spellings are keyed here.  Getting that wrong
    is not a small miss: every api-ms-win-crt-* import would look like a whole
    module with no thunk, which is a completely different diagnosis from the
    one or two symbols actually short."""
    m = {}
    with open(spec_path) as f:
        for line in f:
            g = re.match(r'\s*apiset\s+(\S+)\s*=\s*(\S+)', line)
            if g:
                name, target = g.group(1).lower(), g.group(2).lower()
                m[name] = target
                if not name.endswith('.dll'):
                    m[name + '.dll'] = target
    return m


def guest_thunk_path(src, build, dll):
    """The built x86_64-windows thunk PE for a module name, or None."""
    stem = dll.lower()
    if stem.endswith('.dll'):
        stem = stem[:-4]
    # winspool.drv and friends are built as <name-with-extension>.dll
    for d, f in ((stem, stem + '.dll'), (dll.lower(), dll.lower() + '.dll')):
        p = os.path.join(build, 'dlls', d, 'x86_64-windows', f)
        if os.path.exists(p):
            return p
    return None


def walk(subjects, build, src, sibling_dirs, out=sys.stdout, log=sys.stderr):
    apisets = read_apisets(os.path.join(src, 'dlls', 'apisetschema',
                                        'apisetschema.spec'))
    seen_subject = set()
    exports_cache = {}
    holes = []
    checked = 0
    queue = list(subjects)

    def find_sibling(dll):
        for d in sibling_dirs:
            p = os.path.join(d, dll)
            if os.path.exists(p):
                return p
            # the filesystem may be case-sensitive and an import table carries
            # whatever case the linker wrote
            try:
                for ent in os.listdir(d):
                    if ent.lower() == dll.lower():
                        return os.path.join(d, ent)
            except OSError:
                pass
        return None

    while queue:
        path = queue.pop(0)
        real = os.path.realpath(path)
        if real in seen_subject:
            continue
        seen_subject.add(real)
        try:
            pe = PE(path)
        except ValueError as e:
            print('SUBJECT-BAD\t%s\t%s' % (path, e), file=out)
            holes.append(('-', path, str(e)))
            continue
        if pe.machine != 0x8664:
            print('SUBJECT-MACHINE\t%s\t%#x' % (path, pe.machine), file=out)
            holes.append(('-', path, 'machine %#x is not AMD64' % pe.machine))
            continue
        who = os.path.basename(path)
        print('SUBJECT\t%s' % path, file=log)

        for dll, syms in pe.imports():
            target = apisets.get(dll.lower(), dll).lower()
            note = '' if target == dll.lower() else ' (apiset -> %s)' % target

            sib = find_sibling(target)
            if sib and os.path.realpath(sib) not in seen_subject:
                queue.append(sib)
            if sib:
                serve = sib
            else:
                serve = guest_thunk_path(src, build, target)
            if serve is None:
                print('NO-THUNK\t%s\t%s\t*' % (who, target), file=out)
                holes.append((who, target, '<whole module: no guest thunk built>'))
                continue
            if serve not in exports_cache:
                try:
                    exports_cache[serve] = PE(serve).exports()
                except ValueError as e:
                    print('NO-THUNK\t%s\t%s\t*' % (who, target), file=out)
                    holes.append((who, target, 'unreadable: %s' % e))
                    exports_cache[serve] = (set(), set())
                    continue
            names, ords = exports_cache[serve]
            print('  %-34s -> %-24s%s  %d name(s)'
                  % (dll, os.path.basename(serve), note, len(syms)), file=log)
            for s in syms:
                checked += 1
                if isinstance(s, tuple):
                    if s[1] not in ords:
                        print('HOLE\t%s\t%s\t#%d' % (who, target, s[1]), file=out)
                        holes.append((who, target, '#%d' % s[1]))
                elif s is None:
                    continue
                elif s not in names:
                    print('HOLE\t%s\t%s\t%s' % (who, target, s), file=out)
                    holes.append((who, target, s))

    print('SUMMARY\t%d import(s) checked across %d module(s)\t%d hole(s)'
          % (checked, len(seen_subject), len(holes)), file=out)
    return holes


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('subject', nargs='+', help='third-party x86-64 PE(s) to walk')
    ap.add_argument('--build', required=True, help='the Wine BUILD directory')
    ap.add_argument('--src', required=True, help='the Wine SOURCE directory')
    ap.add_argument('--sibling-dir', action='append', default=[],
                    help='directory whose real PEs may satisfy an import '
                         '(the application directory); repeatable')
    a = ap.parse_args()
    sib = a.sibling_dir or [os.path.dirname(os.path.abspath(a.subject[0]))]
    walk(a.subject, a.build, a.src, sib)
    return 0


if __name__ == '__main__':
    sys.exit(main())
