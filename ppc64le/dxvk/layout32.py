#!/usr/bin/env python3
"""Which D3D11/DXGI aggregates does an i386 guest lay out differently?

The 64-bit dxvk lane passes descriptor-struct pointers straight through to
native code because x86-64 and ppc64le lay all 297 of them out identically.
An i386 guest does not get that guarantee: pointer members shrink to 4 bytes
and alignment drops to 4.  This scan re-derives the divergent list instead of
trusting a hand-maintained one — docs/i386-lane-design.md carries the measured
result (47 of 297) and what each divergence costs.

Method: collect every `} D3D11_*;` / `} DXGI_*;` typedef close from Wine's own
generated headers, emit one source with a flat sizeof/alignof array, compile it
for x86_64-windows-gnu and i386-windows-gnu with clang (no CRT needed — the
values are read back out of -S assembly, never executed), and diff.

Run on a machine with the Wine tree's generated headers (op4k's checkout):

    ./layout32.py [wine-tree-root]     # default: ../../ from this file
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WINE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                       else os.path.join(HERE, "..", ".."))
INC = os.path.join(WINE, "include")

HEADER_RE = re.compile(r"(d3d11(_\d)?|d3d11on12|dxgi1?_?\d*)\.h$")
NAME_RE = re.compile(r"\}\s*(D3D11_\w+|DXGI_\w+)\s*;")


def collect():
    names = set()
    for fn in os.listdir(INC):
        if not HEADER_RE.match(fn):
            continue
        with open(os.path.join(INC, fn), errors="replace") as fh:
            for m in NAME_RE.finditer(fh.read()):
                names.add(m.group(1))
    return sorted(names)


def emit(names, path):
    with open(path, "w") as fh:
        fh.write("#define WIN32_LEAN_AND_MEAN\n#define COBJMACROS\n"
                 "#include <windows.h>\n#include <d3d11_4.h>\n"
                 "#include <d3d11on12.h>\n#include <dxgi1_6.h>\n"
                 "const unsigned int layout[] = {\n")
        for n in names:
            fh.write(f"    (unsigned int)sizeof({n}),"
                     f" (unsigned int)__alignof__({n}),\n")
        fh.write("};\n")


def values(src, target):
    asm = src[:-2] + f"-{target}.s"
    subprocess.run(
        ["clang", "-target", f"{target}-windows-gnu", "-nostdlibinc",
         f"-I{INC}", f"-I{os.path.join(INC, 'msvcrt')}", "-D_UCRT",
         "-O0", "-S", "-o", asm, src],
        check=True)
    text = open(asm).read()
    m = re.search(r"^_?layout:\n((?:\s+\.long\s+\d+[^\n]*\n)+)", text, re.M)
    return [int(x) for x in re.findall(r"\.long\s+(\d+)", m.group(1))]


def main():
    names = collect()
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "layout32.c")
        emit(names, src)
        a = values(src, "x86_64")
        b = values(src, "i386")
    assert len(a) == len(b) == 2 * len(names)
    div = [(n, a[2*i], a[2*i+1], b[2*i], b[2*i+1])
           for i, n in enumerate(names)
           if (a[2*i], a[2*i+1]) != (b[2*i], b[2*i+1])]
    print(f"{len(div)} of {len(names)} aggregates diverge (size/align, 64 vs 32):")
    for d in div:
        print("  %-56s 64:%d/%d  32:%d/%d" % d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
