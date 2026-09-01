#!/usr/bin/env python3
"""flat-tier-rows.py -- list a built guest thunk DLL's rows by SIGNATURE TIER.

The tier a flat export's signature came from now rides in the emitted
descriptor: bit 10 (wine_sig.DESC_SRCTIER / THUNK_SIG_SRCTIER) set means the
signature was read from the module's own implementing C DEFINITION -- the
2,624 exports commit 1edc93608b6 newly served -- rather than from a Wine
header declaration.

WHY READ THE BUILT PE RATHER THAN A REPORT.  spec2thunk's --report has carried
provenance since the tier landed, but the BUILD NEVER PASSES --report (see the
Makefile rule for any x86_64-windows/*.dll), so there is no report beside the
artifact a run actually loads, and regenerating one costs a full clang pass
per module.  The bit is in the shipped file.  Reading it there is fast, needs
no toolchain, and answers about the exact bytes the levers will act on --
which is the only artifact whose answer is worth anything to a bisect leg.

  flat-tier-rows.py dlls/ucrtbase/x86_64-windows/ucrtbase.dll
      -> `ucrtbase.dll!Export` for every SOURCE-TIER row, one per line: a
         WINEEMUNOFLATROWS `@file` as it stands.

  --header-tier   list the header-tier rows instead (the control set: no
                  lever may ever reach one of these).
  --count         one summary line per file instead of the names.
  --plain         bare export names, no `module!` prefix.

Exit 0 with rows, 0 with none, 2 if a file is not a thunk module at all.
"""

import struct
import sys

THUNK_SIG_SRCTIER = 0x400
THUNK_INFO_VERSION = 8


class NotAThunkModule(Exception):
    pass


class PE:
    """Just enough PE to read one data directory and follow RVAs.

    Deliberately not a general reader: it refuses anything it does not
    understand rather than guessing, because a wrong answer here would be a
    bisect leg's probe naming an export that is not the one it thinks.
    """

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b'MZ':
            raise NotAThunkModule('not a PE image (no MZ)')
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise NotAThunkModule('not a PE image (no PE signature)')
        machine, nsec, _, _, _, opt_size = struct.unpack_from('<HHIIIH', d, pe + 4)
        self.machine = machine
        opt = pe + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        if magic == 0x20b:      # PE32+
            ndirs = struct.unpack_from('<I', d, opt + 108)[0]
            dirs = opt + 112
        elif magic == 0x10b:    # PE32
            ndirs = struct.unpack_from('<I', d, opt + 92)[0]
            dirs = opt + 96
        else:
            raise NotAThunkModule('unknown optional header magic %#x' % magic)
        self.dirs = [struct.unpack_from('<II', d, dirs + 8 * i)
                     for i in range(ndirs)]
        self.sections = []
        sec = opt + opt_size
        for i in range(nsec):
            _name, vsize, vaddr, rsize, raddr = struct.unpack_from('<8sIIII', d, sec + 40 * i)
            self.sections.append((vaddr, max(vsize, rsize), raddr, rsize))

    def off(self, rva, need=1):
        """RVA -> file offset, or None when it lands outside every section."""
        for vaddr, vsize, raddr, rsize in self.sections:
            if vaddr <= rva < vaddr + vsize:
                o = raddr + (rva - vaddr)
                # raw data can be shorter than the virtual size (a .bss tail);
                # a read past it is not an answer, so say so
                if rva - vaddr + need > rsize:
                    return None
                return o
        return None

    def u32(self, rva):
        o = self.off(rva, 4)
        return None if o is None else struct.unpack_from('<I', self.data, o)[0]

    def asciz(self, rva):
        o = self.off(rva)
        if o is None:
            return None
        end = self.data.index(b'\0', o)
        return self.data[o:end].decode('ascii', 'replace')


def thunk_rows(path):
    """-> (module name, [(export name, descriptor), ...]).  Raises
    NotAThunkModule for anything that is not one."""
    pe = PE(path)
    if not pe.dirs or not pe.dirs[0][0]:
        raise NotAThunkModule('no export directory')
    exp_rva, _size = pe.dirs[0]
    o = pe.off(exp_rva, 40)
    if o is None:
        raise NotAThunkModule('export directory RVA is not in any section')
    (_flags, _stamp, _maj, _min, name_rva, _base, nfunc, nnames,
     func_rva, names_rva, ords_rva) = struct.unpack_from('<IIHHIIIIIII', pe.data, o)
    modname = pe.asciz(name_rva) or '<unnamed>'

    info_rva = None
    for i in range(nnames):
        nm = pe.asciz(pe.u32(names_rva + 4 * i))
        if nm == '__wine_thunk_info':
            ordinal = struct.unpack_from('<H', pe.data, pe.off(ords_rva + 2 * i, 2))[0]
            if ordinal >= nfunc:
                raise NotAThunkModule('__wine_thunk_info ordinal out of range')
            info_rva = pe.u32(func_rva + 4 * ordinal)
            break
    if info_rva is None:
        raise NotAThunkModule('exports no __wine_thunk_info')

    # struct thunk_info, dlls/ntdll/signal_ppc64.c: version, count, stubs_rva,
    # stride, names_rva, sigs_rva, ...
    version = pe.u32(info_rva)
    if version != THUNK_INFO_VERSION:
        raise NotAThunkModule('thunk info version %s, this tool knows %d'
                              % (version, THUNK_INFO_VERSION))
    count = pe.u32(info_rva + 4)
    tnames_rva = pe.u32(info_rva + 16)
    sigs_rva = pe.u32(info_rva + 20)

    rows = []
    for i in range(count):
        sig = pe.u32(sigs_rva + 4 * i)
        nm = pe.asciz(pe.u32(tnames_rva + 4 * i))
        if sig is None or nm is None:
            raise NotAThunkModule('row %d does not read; the file is truncated' % i)
        rows.append((nm, sig))
    return modname, rows


def main(argv):
    want_header = '--header-tier' in argv
    want_count = '--count' in argv
    plain = '--plain' in argv
    files = [a for a in argv if not a.startswith('--')]
    if not files:
        sys.stderr.write(__doc__)
        return 2

    bad = 0
    for path in files:
        try:
            modname, rows = thunk_rows(path)
        except (NotAThunkModule, OSError, struct.error, ValueError) as ex:
            sys.stderr.write('%s: %s\n' % (path, ex))
            bad = 2
            continue
        src = [n for n, s in rows if s & THUNK_SIG_SRCTIER]
        hdr = [n for n, s in rows if not (s & THUNK_SIG_SRCTIER)]
        if want_count:
            print('%-24s %5d rows  %5d source-tier  %5d header-tier'
                  % (modname, len(rows), len(src), len(hdr)))
            continue
        for n in (hdr if want_header else src):
            print(n if plain else '%s!%s' % (modname, n))
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
