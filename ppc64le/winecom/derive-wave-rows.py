#!/usr/bin/env python3
"""derive-wave-rows.py -- where WINEEMUNOCOMWAVE's membership comes from.

The completeness landings 74591109c3f..c199f79caf9 turned hundreds of
previously-REFUSED COM rows into served ones in a single stretch, with no
runtime lever to put any of them back.  When the Witcher 3 stopped loading
afterwards, bisecting that stretch cost seven seat runs
(ppc64le/docs/sessions/2026-09-01/w3-load-regression-bisect.md).  The three
waves below are the theory legs that bisect names, expressed as row and IID
sets a single environment variable can restore to their pre-landing
behaviour.

THE MEMBERSHIP IS DERIVED FROM GIT, NOT WRITTEN BY HAND.  Two commits:

    OLD = 984c52a6d1d   the last known-good W3 baseline
    NEW = c199f79caf9   the head of the completeness stretch

Both revisions' GENERATED marshal headers (dlls/*/[a-z]*_marshal.h) are
parsed into rows keyed by (slot-array name, index inside it) -- not by slot
NAME, because a name repeats across interfaces: ID3D10Device::PSGetShader
lives in both the ID3D10Device and the ID3D10Device1 tables.  Two rules
produce the sets:

  RULE 1 (refuse-string)  A row whose winecom_slot.refuse went from a reason
     STRING at OLD to NULL at NEW is a row the wave newly SERVES.  This is
     the rule the bisect prompt describes, and it is what syscom and dinput8
     are built from.

  RULE 2 (caux-at-0)  A row whose winecom_slot.caux went from NULL at OLD to
     a real count-parameter array at NEW.  These rows carried NO refuse
     string at either revision -- they were refused AT RUNTIME, by the
     dispatcher, because a CA_IFACE_ARR_OUT_STATIC parameter with no count
     map reads its count from parameter 0 and bails.  OMGetRenderTargets is
     the whole reason this rule exists: the bisect's theory 1 is about it,
     and rule 1 alone does not see it, because nothing about its refuse
     field ever changed.  Say so out loud rather than shipping a hand-added
     row.

Interface-level membership uses a third rule: an IID present in NEW's
winecom_iface roster and absent from OLD's is newly rostered, and before the
landing an interface handed out under it got the release-and-NULL treatment
winecom_wrap_out_iface still gives an unknown IID.  {77aa99a0}
IAudioSessionManager2 is the one the Witcher 3 notes name.

Usage
-----
    ./derive-wave-rows.py                 # rewrite wave-rows.list from git
    ./derive-wave-rows.py --emit-header   # rewrite libs/winecom/winecom_waves.h
                                          #   from wave-rows.list
    ./derive-wave-rows.py --check         # both, to stdout, and diff against
                                          #   what is checked in (exit 1 on drift)

Run it from anywhere; paths are resolved against this file.
"""
import os
import re
import subprocess
import sys

OLD = '984c52a6d1d'
NEW = 'c199f79caf9'

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
LIST = os.path.join(HERE, 'wave-rows.list')
HEADER = os.path.join(ROOT, 'libs', 'winecom', 'winecom_waves.h')

# The headers each wave is derived from.  d3d12/d3d9/dsound/xaudio2 are read
# too (see the report at the bottom) but no alias claims them.
HEADERS = [
    'dlls/combase/syscom_marshal.h',
    'dlls/d3d11/d3d11_marshal.h',
    'dlls/d3d12/d3d12_marshal.h',
    'dlls/d3d9/d3d9_marshal.h',
    'dlls/dinput8/dinput8_marshal.h',
    'dlls/dsound/dsound_marshal.h',
    'dlls/mfplat/mf_marshal.h',
    'dlls/xaudio2_8/xaudio2_marshal.h',
    'dlls/xaudio2_9/xaudio2_marshal.h',
]

SLOT_DECL = re.compile(r'static const struct winecom_slot\s+(\w+)\s*\[[^\]]*\]\s*=')
IFACE_DECL = re.compile(r'static const struct winecom_iface\s+(\w+)\s*\[[^\]]*\]\s*=')

# winecom_slot field indices, counting from the name at 0
F_REFUSE = 1
F_CAUX = 8


def show(commit, path):
    return subprocess.run(['git', 'show', f'{commit}:{path}'],
                          capture_output=True, text=True, cwd=ROOT).stdout


def _skip_string(text, i):
    """i points at the opening quote; -> index just past the closing one."""
    i += 1
    while i < len(text):
        if text[i] == '\\':
            i += 2
            continue
        if text[i] == '"':
            return i + 1
        i += 1
    return i


def array_bodies(text, decl_re):
    """-> [(array_name, body_between_the_outer_braces)]"""
    out = []
    for m in decl_re.finditer(text):
        start = text.index('{', m.end())
        depth, i = 0, start
        while i < len(text):
            c = text[i]
            if c == '"':
                i = _skip_string(text, i)
                continue
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append((m.group(1), text[start + 1:i]))
    return out


def split_rows(body):
    """Brace-balanced, string-aware split of an initializer body into its
    top-level { ... } elements, each a list of its top-level fields."""
    rows, depth, cur, field, i = [], 0, [], [], 0
    while i < len(body):
        c = body[i]
        if c == '"':
            j = _skip_string(body, i)
            field.append(body[i:j])
            i = j
            continue
        if c == '{':
            depth += 1
            if depth == 1:
                field, cur = [], []
                i += 1
                continue
        elif c == '}':
            depth -= 1
            if depth == 0:
                cur.append(''.join(field).strip())
                rows.append(cur)
                i += 1
                continue
        elif c == ',' and depth == 1:
            cur.append(''.join(field).strip())
            field = []
            i += 1
            continue
        field.append(c)
        i += 1
    return rows


def rows_of(text):
    """-> {(array, index): [fields]}"""
    out = {}
    for arr, body in array_bodies(text, SLOT_DECL):
        for idx, fields in enumerate(split_rows(body)):
            if len(fields) > F_REFUSE:
                out[(arr, idx)] = fields
    return out


def ifaces_of(text):
    """-> ({iid_text: (iface_name, slots_array)}, {slots_array: iface_name})"""
    by_iid, arr2iface = {}, {}
    for _arr, body in array_bodies(text, IFACE_DECL):
        for fields in split_rows(body):
            if len(fields) < 4:
                continue
            name = fields[0].strip().strip('"')
            nums = re.findall(r'0x[0-9a-fA-F]+', fields[1])
            slots = fields[3].strip()
            if len(nums) != 11:
                continue
            d1, d2, d3 = (int(x, 16) for x in nums[:3])
            b = [int(x, 16) for x in nums[3:]]
            iid = '{%08x-%04x-%04x-%02x%02x-%s}' % (
                d1, d2, d3, b[0], b[1], ''.join('%02x' % x for x in b[2:]))
            by_iid[iid] = (name, slots)
            if slots != 'NULL':
                arr2iface[slots] = name
    return by_iid, arr2iface


def qualify(iface_name, slot_name):
    """The Iface::Slot spelling the lever matches on.  A row's own name
    already carries the interface a slot was DECLARED on, which for an
    inherited slot is a base interface (ID3D10Device::PSGetShader inside the
    ID3D10Device1 table); the lever accepts either spelling, and the list
    carries the one naming the interface the row actually lives on, because
    that is the one a reader can find in the roster."""
    tail = slot_name.split('::')[-1] if slot_name else slot_name
    return f'{iface_name}::{tail}'


def analyse():
    """-> (per_file_newly_served, per_file_caux_at_0, per_file_new_iids)"""
    served, caux0, newiids = {}, {}, {}
    for path in HEADERS:
        told, tnew = show(OLD, path), show(NEW, path)
        if not tnew or not told:
            continue
        rold, rnew = rows_of(told), rows_of(tnew)
        iid_old, _ = ifaces_of(told)
        iid_new, a2i = ifaces_of(tnew)

        s, c = set(), set()
        for key, fields in rnew.items():
            old = rold.get(key)
            if not old:
                continue
            name = fields[0].strip().strip('"')
            q = qualify(a2i.get(key[0], '?'), name)
            if old[F_REFUSE].startswith('"') and not fields[F_REFUSE].startswith('"'):
                s.add(q)
            if (len(old) > F_CAUX and len(fields) > F_CAUX
                    and old[F_CAUX] == 'NULL' and fields[F_CAUX] != 'NULL'):
                c.add(q)
        served[path], caux0[path] = s, c
        newiids[path] = [(i, iid_new[i][0]) for i in sorted(iid_new)
                         if i not in iid_old]
    return served, caux0, newiids


def build_waves():
    served, caux0, newiids = analyse()
    d3d11 = 'dlls/d3d11/d3d11_marshal.h'
    combase = 'dlls/combase/syscom_marshal.h'
    dinput8 = 'dlls/dinput8/dinput8_marshal.h'

    # getfamily: RULE 2 over d3d11 (the caux-at-0 fix -- OMGetRenderTargets,
    # SOGetTargets and the whole XSGetShader set) UNION the XSGetShader rows
    # RULE 1 also names (the count-through-pointer class, which changed both
    # its refuse string and its caux in the same landing).  The two rules
    # agree on the GetShader rows; only rule 2 sees the OMGet/SOGet ones.
    get_r1 = {q for q in served[d3d11] if re.search(r'::[A-Z]{2}GetShader$', q)}
    getfamily = sorted(caux0[d3d11] | get_r1)

    # syscom: every combase row rule 1 names, plus every IID the syscom wave
    # newly rostered (before it, winecom_wrap_out_iface released the object
    # and handed back NULL + E_NOINTERFACE).
    syscom_rows = sorted(served[combase])
    syscom_iids = [i for i, _ in newiids[combase]]
    syscom_iid_names = dict(newiids[combase])

    dinput8_rows = sorted(served[dinput8])

    return {
        'getfamily': (getfamily, [], {}),
        'syscom': (syscom_rows, syscom_iids, syscom_iid_names),
        'dinput8': (dinput8_rows, [], {}),
    }, served, caux0, newiids


LIST_HEADER = """\
# wave-rows.list -- WINEEMUNOCOMWAVE's membership, DERIVED, not hand-written.
#
# GENERATED by ppc64le/winecom/derive-wave-rows.py.  Do not edit by hand: run
# the script, which re-derives this file from the two commits named below and
# then regenerates libs/winecom/winecom_waves.h from it.
#
#   OLD = {old}   the last known-good Witcher 3 baseline
#   NEW = {new}   the head of the completeness stretch
#
# A row is in a wave when the generated marshal headers show it changing
# between those two commits, by one of two rules the script's own banner
# spells out in full:
#
#   rule 1  winecom_slot.refuse went from a reason STRING to NULL -- the row
#           was refused at generation time and is now served.
#   rule 2  winecom_slot.caux went from NULL to a real count-parameter array
#           -- the row was refused AT RUNTIME (a CA_IFACE_ARR_OUT_STATIC with
#           no count map) and now serves.  Nothing about its refuse field
#           ever changed, so rule 1 cannot see it; OMGetRenderTargets, the
#           bisect's own theory 1, is exactly this shape.
#
# An `iid` line is an interface the wave newly ROSTERED.  Before it,
# winecom_wrap_out_iface released the object, NULLed the out pointer and
# answered E_NOINTERFACE for that IID; WINEEMUNOCOMIIDS puts that back.
#
# Rows are named Iface::Slot against the interface the row LIVES on.  The
# lever also accepts the row's own declared name (an inherited slot's name
# carries its BASE interface), so either spelling works at the command line.
"""


def emit_list():
    waves, served, caux0, newiids = build_waves()
    out = [LIST_HEADER.format(old=OLD, new=NEW)]
    for name in ('getfamily', 'syscom', 'dinput8'):
        rows, iids, iid_names = waves[name]
        out.append('')
        out.append(f'[{name}]  # {len(rows)} rows, {len(iids)} IIDs')
        for r in rows:
            out.append(f'row {r}')
        for i in iids:
            out.append(f'iid {i}  # {iid_names.get(i, "")}')
    out.append('')
    out.append('# --- not claimed by any alias, for the record -------------------------')
    out.append('# The same two commits also newly serve rows no wave name covers.  They')
    out.append('# are listed here so the reader knows the aliases are not the whole')
    out.append('# landing; name them individually with WINEEMUNOCOMROWS if a leg needs')
    out.append('# them.')
    for path in HEADERS:
        extra = len(served.get(path, ()))
        iid = len(newiids.get(path, ()))
        if path.endswith(('syscom_marshal.h', 'dinput8_marshal.h')):
            continue
        if extra or iid:
            out.append(f'#   {path}: {extra} rows newly served, '
                       f'{iid} interfaces newly rostered')
    out.append('')
    return '\n'.join(out)


def read_list(text):
    waves, cur = {}, None
    for line in text.splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            cur = line[1:-1]
            waves[cur] = ([], [])
            continue
        if cur is None:
            continue
        kind, _, val = line.partition(' ')
        val = val.strip()
        if kind == 'row':
            waves[cur][0].append(val)
        elif kind == 'iid':
            waves[cur][1].append(val)
    return waves


HEADER_TOP = """\
/* GENERATED by ppc64le/winecom/derive-wave-rows.py from
 * ppc64le/winecom/wave-rows.list -- do not edit.
 *
 * WINEEMUNOCOMWAVE's alias table: the row and IID sets each wave name
 * expands to, derived from the generated marshal headers between
 * {old} and {new}.  The .list file carries the derivation rules and the
 * provenance of every entry; this header is only the runtime's copy of it,
 * baked in because a bisect leg has to be ONE environment variable and the
 * runtime is PE-side with no data file to read.
 *
 * Included by libs/winecom/winecom.c after wine/winecom.h.
 */

struct wc_wave
{{
    const char *name;
    const char * const *rows;
    UINT row_count;
    const char * const *iids;
    UINT iid_count;
}};
"""


def emit_header():
    with open(LIST) as f:
        waves = read_list(f.read())
    out = [HEADER_TOP.format(old=OLD, new=NEW)]
    order = [w for w in ('getfamily', 'syscom', 'dinput8') if w in waves]
    for name in order:
        rows, iids = waves[name]
        out.append(f'static const char * const wc_wave_{name}_rows[] =')
        out.append('{')
        for r in rows:
            out.append(f'    "{r}",')
        out.append('};')
        if iids:
            out.append(f'static const char * const wc_wave_{name}_iids[] =')
            out.append('{')
            for i in iids:
                out.append(f'    "{i}",')
            out.append('};')
        out.append('')
    out.append('static const struct wc_wave wc_waves[] =')
    out.append('{')
    for name in order:
        rows, iids = waves[name]
        iid_expr = (f'wc_wave_{name}_iids, {len(iids)}' if iids else 'NULL, 0')
        out.append(f'    {{ "{name}", wc_wave_{name}_rows, '
                   f'{len(rows)}, {iid_expr} }},')
    out.append('};')
    out.append('')
    return '\n'.join(out)


def main():
    args = sys.argv[1:]
    if '--check' in args:
        bad = 0
        for path, gen in ((LIST, emit_list()), (HEADER, emit_header())):
            with open(path) as f:
                have = f.read()
            if have != gen:
                print(f'DRIFT: {path} is not what this script generates',
                      file=sys.stderr)
                bad = 1
        return bad
    if '--emit-header' in args:
        with open(HEADER, 'w') as f:
            f.write(emit_header())
        print(f'wrote {HEADER}')
        return 0
    with open(LIST, 'w') as f:
        f.write(emit_list())
    print(f'wrote {LIST}')
    with open(HEADER, 'w') as f:
        f.write(emit_header())
    print(f'wrote {HEADER}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
