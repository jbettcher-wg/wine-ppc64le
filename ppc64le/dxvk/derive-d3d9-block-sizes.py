#!/usr/bin/env python3
"""derive-d3d9-block-sizes.py -- check dlls/d3d9/main.c's D3DFORMAT block table
against the two in-tree authorities it was derived from, and print the diff.

WHY THIS EXISTS.  dlls/d3d9/main.c's i386 Lock walkers bounce every D3D9
mapping into below-4-GiB memory, and sizing that bounce is block-compressed
mip arithmetic per format: a wrong (blockW, blockH, elementSize) triple
silently corrupts the buffer the bounce exists to protect.  So the triples
are not remembered, they are DERIVED, and this script is the derivation --
run it after touching the table, or after pulling either upstream.

THE TWO AUTHORITIES.

  wined3d  dlls/d3d9/device.c's wined3dformat_from_d3dformat() maps
           D3DFORMAT -> wined3d_format_id (FOURCCs pass through unchanged);
           dlls/wined3d/utils.c's format_block_info[] gives the block triple
           for block and macropixel formats, formats[] the bpp for the rest,
           and typed_formats[] the typeless entry a typed format takes its
           size from.

  DXVK     which is what actually matters, because DXVK is the host that
           answers the Pitch and owns the buffer.  src/d3d9/d3d9_format.cpp
           maps D3D9Format -> VkFormat; src/dxvk/dxvk_format.cpp's
           g_formatInfos gives that VkFormat its elementSize and blockSize;
           and formats DXVK maps to nothing fall to
           D3D9VkFormatTable::GetUnsupportedFormatInfo(), which states an
           elementSize outright.

They agree except where the script says so, and where they disagree DXVK
wins -- it is the host.  Point --dxvk at a DXVK checkout to include the
second derivation; without it only the wined3d one runs.

  ./derive-d3d9-block-sizes.py [--wine <wine-upstream>] [--dxvk <dxvk src/src>]
"""

import argparse, os, re, sys


def fourcc(s):
    return (ord(s[0]) | (ord(s[1]) << 8) | (ord(s[2]) << 16) | (ord(s[3]) << 24))


def brace_block(text, start_marker):
    """the { ... } that follows start_marker, braces balanced."""
    i = text.index(start_marker)
    s = text.index('{', i + len(start_marker))
    depth = 0
    j = s
    while True:
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[s:j + 1]
        j += 1


# ---------------------------------------------------------------- D3DFORMAT
def d3dformats(wine):
    src = open(os.path.join(wine, 'include/d3d9types.h')).read()
    blk = src[src.index('D3DFMT_UNKNOWN'):src.index('D3DFMT_FORCE_DWORD')]
    out = {}
    for m in re.finditer(r"(D3DFMT_\w+)\s*=\s*(MAKEFOURCC\([^)]*\)|[^,\n]+),", blk):
        name, val = m.group(1), m.group(2).strip()
        cc = re.match(r"MAKEFOURCC\(\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*\)", val)
        out[name] = fourcc(''.join(cc.groups())) if cc else int(val, 0)
    # D3D8-only, so d3d9types.h does not name it -- but DXVK does, a D3D8
    # title reaches this lane through d3d8's d3d9 back end, and main.c's
    # table carries it, so it has to be checkable here too.
    out['D3DFMT_W11V11U10'] = 65
    return out


# ------------------------------------------------------------ authority 1
def from_wined3d(wine):
    dev = open(os.path.join(wine, 'dlls/d3d9/device.c')).read()
    fn = dev[dev.index('enum wined3d_format_id wined3dformat_from_d3dformat'):]
    fn = fn[:fn.index('\n}')]
    mapping = dict(re.findall(r'case (D3DFMT_\w+): return (WINED3DFMT_\w+);', fn))

    u = open(os.path.join(wine, 'dlls/wined3d/utils.c')).read()
    bpp, typeless, block = {}, {}, {}
    for line in brace_block(u, 'static const struct wined3d_format_channels formats[]').split('\n'):
        m = re.match(r'\{(WINED3DFMT_\w+)\s*,(.*)\}\s*,\s*$', line.strip())
        if m:
            f = [x.strip() for x in m.group(2).split(',')]
            bpp[m.group(1)] = int(f[8])
    for line in brace_block(u, 'static const struct wined3d_typed_format_info typed_formats[]').split('\n'):
        m = re.match(r'\{(WINED3DFMT_\w+)\s*,\s*(WINED3DFMT_\w+)\s*,', line.strip())
        if m:
            typeless[m.group(1)] = m.group(2)
    for line in brace_block(u, 'static const struct wined3d_format_block_info format_block_info[]').split('\n'):
        m = re.match(r'\{(WINED3DFMT_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,', line.strip())
        if m:
            block[m.group(1)] = tuple(int(m.group(i)) for i in (2, 3, 4))

    def geom(w):
        if w in block:
            return block[w]
        b = bpp.get(w)
        if b is None and w in typeless:
            b = bpp.get(typeless[w])
        return (1, 1, b) if b else None

    out = {}
    for name, val in d3dformats(wine).items():
        # a FOURCC D3DFORMAT is its own wined3d_format_id
        w = 'WINED3DFMT_' + name[len('D3DFMT_'):] if val > 0xffff else mapping.get(name)
        g = geom(w) if w else None
        if g:
            out[val] = g
    for cc, w in (('ATI1', 'WINED3DFMT_ATI1N'), ('ATI2', 'WINED3DFMT_ATI2N'),
                  ('INTZ', 'WINED3DFMT_INTZ')):
        g = geom(w)
        if g:
            out[fourcc(cc)] = g
    return out


# ------------------------------------------------------------ authority 2
def from_dxvk(dxvk, wine):
    fmt = open(os.path.join(dxvk, 'dxvk/dxvk_format.cpp')).read()
    vk = {}
    # g_formatInfos is one flat array whose entries each carry a
    # `// VK_FORMAT_...` comment; brace-match each entry rather than
    # regexing it, because the block and multi-plane ones nest.
    for m in re.finditer(r'//\s*(VK_FORMAT_\w+)\s*\n', fmt):
        name = m.group(1).replace('_KHR', '')
        start = fmt.index('{', m.end())
        depth, j = 0, start
        while True:
            if fmt[j] == '{':
                depth += 1
            elif fmt[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = fmt[start + 1:j]
        el = re.match(r'\s*(\d+)\s*,', body)
        if not el:
            continue
        bs = re.search(r'VkExtent3D\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', body)
        vk[name] = ((int(bs.group(1)), int(bs.group(2)), int(el.group(1)))
                    if bs else (1, 1, int(el.group(1))))

    d9 = open(os.path.join(dxvk, 'd3d9/d3d9_format.cpp')).read()
    mapped = {}
    # the VkFormat may sit a comment line or two below the `return {`
    for m in re.finditer(r'case D3D9Format::(\w+):\s*return\s*\{((?:\s*//[^\n]*\n)*)\s*(VK_FORMAT_\w+)', d9):
        mapped[m.group(1)] = m.group(3).replace('_KHR', '')
    unsupported = {}
    tail = d9[d9.index('GetUnsupportedFormatInfo'):]
    sizes = dict(re.findall(r'static const DxvkFormatInfo (\w+)\s*=\s*\{\s*(\d+)', tail))
    for m in re.finditer(r'case D3D9Format::(\w+):\s*\n\s*return &(\w+);', tail):
        if m.group(2) in sizes:
            unsupported[m.group(1)] = (1, 1, int(sizes[m.group(2)]))

    out = {}
    for name, val in d3dformats(wine).items():
        short = name[len('D3DFMT_'):]
        short = {'NULL_FORMAT': 'NULL'}.get(short, short)
        if short in mapped and mapped[short] in vk:
            out[val] = vk[mapped[short]]
        elif short in unsupported:
            out[val] = unsupported[short]
    for cc, short in (('ATI1', 'ATI1'), ('ATI2', 'ATI2'),
                      ('DF16', 'DF16'), ('DF24', 'DF24'), ('INTZ', 'INTZ')):
        if short in mapped and mapped[short] in vk:
            out[fourcc(cc)] = vk[mapped[short]]
    return out


# ------------------------------------------------------- what main.c claims
def from_main_c(wine):
    src = open(os.path.join(wine, 'dlls/d3d9/main.c')).read()
    tbl = brace_block(src, 'static const struct d3d9_format_geom d3d9_format_geoms[]')
    out = {}
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', tbl):
        out[int(m.group(1))] = tuple(int(m.group(i)) for i in (2, 3, 4))
    for m in re.finditer(r"\{\s*D3D9_FOURCC\('(.)','(.)','(.)','(.)'\)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", tbl):
        out[fourcc(''.join(m.group(i) for i in (1, 2, 3, 4)))] = \
            tuple(int(m.group(i)) for i in (5, 6, 7))
    for m in re.finditer(r'\{\s*D3D9_FMT_(ATI[12])\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', tbl):
        out[fourcc(m.group(1))] = tuple(int(m.group(i)) for i in (2, 3, 4))
    return out


def label(v, names):
    return names.get(v, '0x%08x' % v if v > 0xffff else str(v))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--wine', default=os.path.abspath(os.path.join(here, '..', '..')))
    ap.add_argument('--dxvk', default=os.path.expanduser(
        '~/Development/powerpc64le-ports/dxvk-ppc64le/src/src'))
    a = ap.parse_args()

    names = {v: k for k, v in d3dformats(a.wine).items()}
    for cc in ('ATI1', 'ATI2', 'DF16', 'DF24', 'INTZ'):
        names.setdefault(fourcc(cc), 'D3DFMT_' + cc)

    mine = from_main_c(a.wine)
    wd = from_wined3d(a.wine)
    dx = from_dxvk(a.dxvk, a.wine) if os.path.isdir(a.dxvk) else {}
    if not dx:
        print('note: no DXVK checkout at %s; only the wined3d derivation ran\n' % a.dxvk)

    bad = 0
    print('%-28s %-12s %-12s %-12s' % ('format', 'main.c', 'wined3d', 'dxvk'))
    for v in sorted(mine):
        m, w, d = mine[v], wd.get(v), dx.get(v)
        flag = ''
        if d and d != m:
            flag, bad = '  <-- MISMATCH vs the HOST', bad + 1
        elif not d and w and w != m:
            flag = '  <-- differs from wined3d (DXVK wins; expected for the four named in main.c)'
        elif w and w != m:
            flag = '  (wined3d differs; DXVK agrees, so main.c is right)'
        print('%-28s %-12s %-12s %-12s%s' % (label(v, names), m, w or '--', d or '--', flag))

    missing = [v for v in dx if v not in mine and dx[v][2]]
    if missing:
        print('\nformats DXVK can size that main.c refuses (each refusal is deliberate '
              'unless a title needs it):')
        for v in sorted(missing):
            print('  %-28s %s' % (label(v, names), dx[v]))

    print('\n%d mismatch(es) against DXVK.' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
