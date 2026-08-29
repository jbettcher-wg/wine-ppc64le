#!/usr/bin/env python3
"""
library_sweep.py -- every game in a Steam library, and what this port will do
with it, without launching any of them.

WHY THIS EXISTS
---------------
The way a title's status has been established here so far is: launch it, watch
it die, read the log, fix one thing, repeat.  That does not scale to a library,
and it makes the first five minutes with this port a research project rather
than a setup step.

Almost everything that decides a title's fate is visible in its files:

  * whether it ships x86-64 binaries at all, or is 32-bit only (which needs an
    i386 build and a 32-bit-capable bridge),
  * whether it carries kernel anti-cheat, which will not work here and never
    politely says so at runtime,
  * whether its imports would bind against this tree's guest thunk surface --
    which `import_chain.py` answers exactly, and which is the difference
    between "dies in the loader before its own code runs" and "runs",
  * whether it ships a prerequisite installer that will run before the game.

So this walks the whole library and says so, per title, in one table.  It
launches nothing and writes nothing to any prefix.

WHAT IT CANNOT SEE
------------------
Titles you own but have not installed: Steam keeps those in a binary
`appinfo.vdf` cache and their files are not on disk, so there is nothing to
inspect.  They are counted, not judged.  Install one and re-run.

A verdict here is a statement about STATIC facts, not a promise.  A title with
nothing to report can still hit a runtime gap -- that is what the game list
catalogue records, and what a launch is for.
"""

import argparse, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'thunks'))


# ----------------------------------------------------------------- steam

STEAM_ROOTS = ['~/.local/share/Steam', '~/.steam/steam', '~/.steam/root',
               '~/.var/app/com.valvesoftware.Steam/data/Steam']

KV = re.compile(r'^\s*"([^"]+)"\s*(?:"([^"]*)")?\s*$')


def kv_pairs(path):
    """Every "key" "value" pair in a Valve KeyValues file, flat and in order.

    Flat is enough for the three files read here -- libraryfolders.vdf and
    appmanifest_*.acf both carry the keys this needs at a unique name -- and it
    avoids a KeyValues parser that would have to be right about escaping,
    conditionals and includes to answer questions this simple.
    """
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = KV.match(line)
            if m and m.group(2) is not None:
                out.append((m.group(1), m.group(2)))
    return out


def find_steam_root(explicit):
    if explicit:
        return os.path.expanduser(explicit)
    for cand in STEAM_ROOTS:
        p = os.path.expanduser(cand)
        if os.path.isdir(os.path.join(p, 'steamapps')):
            return p
    return None


def libraries(root):
    """-> ([present library paths], [absent ones]).

    A library on a drive that is not mounted is not an error -- people unplug
    disks -- but it IS the explanation for "Steam lists the game and there is
    nothing on disk", so it is reported rather than skipped.
    """
    vdf = os.path.join(root, 'steamapps', 'libraryfolders.vdf')
    paths = [root]
    if os.path.isfile(vdf):
        paths += [v for k, v in kv_pairs(vdf) if k == 'path']
    present, absent, seen = [], [], set()
    for p in paths:
        p = os.path.normpath(p)
        if p in seen:
            continue
        seen.add(p)
        (present if os.path.isdir(os.path.join(p, 'steamapps')) else absent).append(p)
    return present, absent


def manifests(lib):
    d = os.path.join(lib, 'steamapps')
    for name in sorted(os.listdir(d)):
        if not (name.startswith('appmanifest_') and name.endswith('.acf')):
            continue
        pairs = dict(kv_pairs(os.path.join(d, name)))
        appid = pairs.get('appid') or name[12:-4]
        yield dict(appid=appid, name=pairs.get('name', '?'),
                   installdir=os.path.join(d, 'common', pairs.get('installdir', '')),
                   library=lib)


# Valve's own tools and runtimes live in the same list as games.
TOOL_NAMES = re.compile(r'^(Proton|Steam Linux Runtime|Steamworks|SteamVR|'
                        r'Steam Deck|Proton Hotfix|Proton Experimental)', re.I)


def is_tool(app):
    if TOOL_NAMES.match(app['name']):
        return True
    return os.path.isfile(os.path.join(app['installdir'], 'toolmanifest.vdf'))


# ------------------------------------------------------------------- PE

def pe_machine(path):
    """-> the COFF machine word, or None if this is not a PE at all."""
    try:
        with open(path, 'rb') as f:
            if f.read(2) != b'MZ':
                return None
            f.seek(0x3c)
            off = struct.unpack('<I', f.read(4))[0]
            f.seek(off)
            if f.read(4) != b'PE\0\0':
                return None
            return struct.unpack('<H', f.read(2))[0]
    except OSError:
        return None


MACHINE = {0x8664: 'x86-64', 0x14c: '32-bit', 0xaa64: 'arm64', 0x1c4: 'arm'}

# Binaries that are not the game: prerequisite installers, crash handlers,
# and the anti-cheat's own setup.  Naming them keeps the "which exe would
# Steam run" guess from landing on vcredist.
NOT_THE_GAME = re.compile(
    r'(prereq|vcredist|vc_redist|dxsetup|directx|dotnetfx|oalinst|'
    r'crashhandler|crashreport|crashsender|unitycrashhandler|'
    r'easyanticheat_setup|beservice|besetup|installscript|uninstall|'
    r'dxwebsetup|physx|touchup)', re.I)

ANTICHEAT = [
    ('EasyAntiCheat', re.compile(r'(^|[\\/])easyanticheat', re.I)),
    ('EasyAntiCheat (EOS)', re.compile(r'easyanticheat_eos', re.I)),
    ('BattlEye', re.compile(r'(^|[\\/])(battleye|beclient|beservice)', re.I)),
    ('nProtect GameGuard', re.compile(r'gameguard', re.I)),
    ('Xigncode', re.compile(r'xigncode', re.I)),
    ('Vanguard', re.compile(r'vgk\.sys', re.I)),
]

PREREQ = re.compile(r'(ue\d?prereqsetup|vcredist|vc_redist|dxsetup|dotnetfx)', re.I)


def scan_files(root, max_depth=4, limit=40000):
    """Every file under root, bounded.  Games are large; this is not a search."""
    out, n = [], 0
    root = os.path.normpath(root)
    base_depth = root.count(os.sep)
    for dirpath, dirnames, filenames in os.walk(root):
        if dirpath.count(os.sep) - base_depth >= max_depth:
            dirnames[:] = []
        for f in filenames:
            out.append(os.path.join(dirpath, f))
            n += 1
            if n >= limit:
                return out
    return out


def classify(app):
    """-> dict of static facts about one installed title."""
    d = app['installdir']
    facts = dict(exes=[], anticheat=[], prereq=[], missing=False)
    if not os.path.isdir(d):
        facts['missing'] = True
        return facts
    files = scan_files(d)
    for path in files:
        rel = os.path.relpath(path, d)
        for label, rx in ANTICHEAT:
            if rx.search(rel) and label not in facts['anticheat']:
                facts['anticheat'].append(label)
        if PREREQ.search(os.path.basename(path)):
            facts['prereq'].append(rel)
        if path.lower().endswith('.exe'):
            m = pe_machine(path)
            if m is None:
                continue
            facts['exes'].append(dict(rel=rel, path=path, machine=m,
                                      depth=rel.count(os.sep),
                                      game=not NOT_THE_GAME.search(rel)))
    return facts


def launch_candidates(facts):
    """The exes that plausibly ARE the game: shallowest first, installers out."""
    games = [e for e in facts['exes'] if e['game']]
    games.sort(key=lambda e: (e['depth'], len(e['rel'])))
    return games


# --------------------------------------------------------------- verdict

def audit(paths, src, build):
    """import_chain's answer, as (fatal module list, missing export count).

    EVERY subject's own directory is a sibling directory, not just the first
    one's.  A title that ships more than one build of itself puts each build's
    private DLLs beside that build: The Witcher 3 has bin/x64/witcher3.exe and
    bin/x64_dx12/witcher3.exe, and sl.interposer.dll, libxess.dll and the
    GFSDK_Aftermath/SSAO_D3D12 pair exist ONLY under x64_dx12.  Deriving one
    sibling dir from subjects[0] and applying it to both made the DX12 build's
    own shipped DLLs invisible, and the title -- which plays -- was reported
    WILL NOT LOAD, the verdict reserved for a module that kills the process
    before its entry point.  A tool that condemns working titles does not get
    used.
    """
    import import_chain
    paths = list(paths)
    siblings = sorted({os.path.dirname(p) for p in paths})
    holes = import_chain.walk(paths, build, src, siblings,
                              out=open(os.devnull, 'w'), log=open(os.devnull, 'w'))
    modules, exports = [], 0
    for who, target, what in holes:
        if what.startswith('<whole module'):
            if target not in modules:
                modules.append(target)
        else:
            exports += 1
    return modules, exports


def build_facts(args):
    """What THIS build and THIS machine can do, which decides some verdicts.

    A title being 32-bit is only a problem if the tree was built without the
    i386 lane or the bridge cannot start a 32-bit process -- and both of those
    are properties of the setup, not of the game, so they are checked once and
    reported once rather than guessed at per title.
    """
    out = dict(i386=False, bridge=None, bridge32=False, pagesize=None)
    out['i386'] = os.path.isfile(os.path.join(args.build, 'dlls', 'ntdll',
                                              'i386-windows', 'ntdll.dll'))
    try:
        out['pagesize'] = os.sysconf('SC_PAGE_SIZE')
    except (ValueError, OSError):
        pass
    cands = []
    if os.environ.get('WINEFEXBRIDGE'):
        cands.append(os.environ['WINEFEXBRIDGE'])
    try:
        with open('/proc/sys/fs/binfmt_misc/FEX-x86_64') as f:
            for line in f:
                if line.startswith('interpreter'):
                    d = os.path.dirname(os.path.dirname(line.split()[1]))
                    cands.append(os.path.join(d, 'Source', 'Tools', 'FexBridge',
                                              'libfexbridge.so'))
    except OSError:
        pass
    # Where build-fexbridge.sh installs it, which is where ntdll's loader looks.
    # The old candidate here named a fex-ppc64le build tree; fastppcx86 is the
    # emulator now and that tree has no binaries, so this reported "no bridge
    # found" on a machine with a working one.
    cands.append(os.path.join(args.build, 'dlls', 'ntdll', 'libfexbridge.so'))
    for c in cands:
        if os.path.isfile(c):
            out['bridge'] = c
            try:
                with open(c, 'rb') as f:
                    out['bridge32'] = b'fexbridge_process_init32' in f.read()
            except OSError:
                pass
            if out['bridge32']:
                break
    return out


def verdict(app, facts, args):
    """-> (tag, note).  Tags are ordered worst-first by the caller."""
    if facts['missing']:
        return 'NOT-ON-DISK', 'the manifest is here but the files are not'
    if facts['anticheat']:
        return 'ANTI-CHEAT', '%s -- kernel anti-cheat does not work here' % \
            ', '.join(facts['anticheat'])
    cands = launch_candidates(facts)
    if not cands:
        if facts['exes']:
            return 'NO GAME EXE', 'only installers and helpers found'
        return 'NO WINDOWS EXE', 'a native Linux build, or nothing installed'
    machines = {e['machine'] for e in cands}
    is32 = 0x8664 not in machines
    if is32:
        only = ', '.join(sorted(MACHINE.get(m, hex(m)) for m in machines))
        if not any(m in (0x14c, 0x8664) for m in machines):
            return 'WRONG MACHINE', 'candidates are %s, which this port does not ' \
                                    'run' % only
        missing = []
        if not args.machine['i386']:
            missing.append('the tree has no i386 lane (configure with '
                           '--enable-archs=ppc64,i386)')
        if not args.machine['bridge32']:
            missing.append('no bridge with fexbridge_process_init32')
        if missing:
            return '32-BIT: BLOCKED', '; '.join(missing)
        if not args.audit:
            return '32-BIT', 'runs through the WoW64 lane; this setup has it'
        # This setup CAN run it and import_chain.py can now read a PE32
        # subject, so give it the same real verdict a 64-bit title gets
        # rather than stopping at "it will start".
    note = ''
    if facts['prereq']:
        note = 'ships a prerequisite installer (%s)' % \
               os.path.basename(facts['prereq'][0])
    if not is32 and not args.audit:
        return 'UNAUDITED', note or 'run again with --audit to check its imports'
    want_machine = 0x14c if is32 else 0x8664
    subjects = [e['path'] for e in cands if e['machine'] == want_machine][:args.max_exes]
    try:
        modules, exports = audit(subjects, args.src, args.build)
    except Exception as e:                       # a malformed PE is a finding
        return 'UNREADABLE', 'import audit failed: %s' % e
    bit = '32-bit, WoW64 lane: ' if is32 else ''
    if modules:
        return 'WILL NOT LOAD', bit + 'no guest thunk for %s%s' % (
            ', '.join(modules[:4]), ' and %d more' % (len(modules) - 4)
            if len(modules) > 4 else '')
    if exports:
        return 'MISSING EXPORTS', bit + '%d import(s) bind to a sentinel; fatal ' \
                                  'only if called%s' % (exports, '; ' + note if note else '')
    return 'READY', bit + (note or 'imports all bind')


ORDER = ['ANTI-CHEAT', 'WRONG MACHINE', 'WILL NOT LOAD', '32-BIT: BLOCKED',
         'MISSING EXPORTS', 'UNREADABLE', 'NO GAME EXE', 'NO WINDOWS EXE',
         'NOT-ON-DISK', '32-BIT', 'UNAUDITED', 'READY']


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--steam-root', help='override Steam\'s data directory')
    ap.add_argument('--src', default=os.path.join(HERE, '..', '..'),
                    help='the Wine source tree')
    ap.add_argument('--build', help='the Wine build directory (default: --src)')
    ap.add_argument('--audit', action='store_true',
                    help='also resolve each title\'s imports against this '
                         'tree\'s guest thunks (slower, and the answer that '
                         'matters)')
    ap.add_argument('--max-exes', type=int, default=2,
                    help='how many candidate binaries per title to audit')
    ap.add_argument('--appid', action='append',
                    help='only these appids (repeatable)')
    args = ap.parse_args()
    args.src = os.path.abspath(os.path.expanduser(args.src))
    args.build = os.path.abspath(os.path.expanduser(args.build or args.src))

    args.machine = build_facts(args)
    m = args.machine
    print('This build: %s, %s, page size %s' % (
        'i386 lane present' if m['i386'] else 'NO i386 lane (32-bit titles blocked)',
        ('bridge %s 32-bit-capable' % ('is' if m['bridge32'] else 'is NOT'))
        if m['bridge'] else 'no bridge found',
        m['pagesize'] if m['pagesize'] else '?'))
    if m['pagesize'] and m['pagesize'] != 4096:
        print('  *** this kernel has %d-byte pages; PE images need 4096 and '
              'nothing will run ***' % m['pagesize'])
    if m['bridge']:
        print('  bridge: %s' % m['bridge'])

    root = find_steam_root(args.steam_root)
    if not root:
        print('no Steam installation found; pass --steam-root', file=sys.stderr)
        return 2
    present, absent = libraries(root)
    print('Steam root: %s' % root)
    for p in present:
        print('  library:  %s' % p)
    for p in absent:
        print('  library:  %s  -- NOT MOUNTED, its games cannot be inspected' % p)

    apps, tools = [], 0
    for lib in present:
        for app in manifests(lib):
            if is_tool(app):
                tools += 1
                continue
            if args.appid and app['appid'] not in args.appid:
                continue
            apps.append(app)
    print('  %d installed title(s), %d Valve tool(s)/runtime(s) skipped\n'
          % (len(apps), tools))

    rows = []
    for app in apps:
        facts = classify(app)
        tag, note = verdict(app, facts, args)
        rows.append((ORDER.index(tag) if tag in ORDER else 99, tag, app, note))
    rows.sort(key=lambda r: (r[0], r[2]['name'].lower()))

    print('%-8s %-38s %-16s %s' % ('APPID', 'TITLE', 'VERDICT', 'WHY'))
    print('%-8s %-38s %-16s %s' % ('-' * 8, '-' * 38, '-' * 16, '-' * 40))
    for _, tag, app, note in rows:
        print('%-8s %-38.38s %-16s %s' % (app['appid'], app['name'], tag, note))

    counts = {}
    for _, tag, _, _ in rows:
        counts[tag] = counts.get(tag, 0) + 1
    print('\n' + ', '.join('%d %s' % (n, t) for t, n in
                           sorted(counts.items(), key=lambda kv: ORDER.index(kv[0])
                                  if kv[0] in ORDER else 99)))
    if not args.audit:
        print('\nRun again with --audit for the import check, which is the '
              'verdict that decides whether a title reaches its own code.')
    print('\nTitles you own but have not installed are not listed: Steam keeps '
          'those in a binary cache and their files are not on disk.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
