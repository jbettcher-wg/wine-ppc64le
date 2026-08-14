#!/usr/bin/env python3
"""
wine_sig.py -- derive a thunk signature descriptor from Wine's OWN headers,
using clang as the oracle.

Why not the .spec files: a Wine .spec carries no return type at all (only a
-ret64 flag) and its ARG_LONG is 32 bits, so `@ stdcall Sleep(long)` and
`@ stdcall GetModuleHandleW(wstr)` and `@ cdecl floor(double)` are all
indistinguishable from what a marshalling thunk needs to know.  The sibling
project wine-spec-thunk hit exactly this.  So: headers, not specs.

Why not a regex over the headers: the Win32 type system is ~2000 typedefs deep
with #if branches (ULONG_PTR alone has four definitions).  Reimplementing that
resolution is where the guessing creeps back in.  Instead this drives clang
twice:

  stage 1 (structure)  -- parse Wine's real windows.h for the x86_64-windows
                          target and read the FunctionDecl out of clang's JSON
                          AST: declaring file and line, return spelling,
                          parameter spellings, variadic flag.

  stage 2 (semantics)  -- emit a probe that reconstructs the prototype from
                          those spellings, assigns the real function to it
                          (so a wrong reconstruction is a hard compile error),
                          and _Static_asserts that every parameter and the
                          return are integer/pointer class and fit in 64 bits.
                          Compile it with -Werror.  Compiles => representable.
                          Fails => REFUSED, with the compiler's own message.

Nothing here defaults, widens or assumes.  A type this cannot prove is
register-passable is refused, never described.

---------------------------------------------------------------------------
va_list IS ALLOWED HERE.  THIS IS AN ABI-PAIR-SPECIFIC ALLOWANCE.  MEASURED.
---------------------------------------------------------------------------
The sibling project (wine-spec-thunk, gen_spec_thunk.py) refuses any va_list
argument outright -- "va_list argument: not expressible in the spec".  That
blanket refusal was written for a different boundary and does NOT apply to
the guest-MS-x64 -> host-ppc64le-ELFv2 pair:

  * MS-x64 va_list is `char *` (clang's __builtin_ms_va_list; see Wine's
    include/msvcrt/vadefs.h).  It points into a contiguous run of 8-byte
    argument slots: the callee homes RCX/RDX/R8/R9 into the shadow space, so
    the slots are contiguous from the first vararg onward, and floats are
    promoted to 8-byte doubles.
  * ppc64le ELFv2 va_list is likewise a plain pointer into the parameter
    save area -- 8 bytes, no register-save-area half to reconstruct.
    Measured on the AC922: a native ppc64le program handed va_arg a flat,
    hand-built, 8-bytes-per-slot buffer and read back long, long, pointer,
    double and int correctly.

So a guest va_list travels through as one ordinary pointer-sized integer
argument, with no translation, for integer, pointer and floating-point
varargs.  Nothing special is needed in this file for that: va_list already
classifies as pointer class with sizeof == 8, so the stage-2 probe accepts
it on its own.  This note exists so nobody later "fixes" it back.

Still genuinely unsupported, still refused:
  * a `long double` vararg -- 128-bit on ppc64, 64-bit on MS-x64, so the
    slot widths differ and the walk desynchronises.
  * a struct passed BY VALUE as a vararg -- MS-x64 passes a pointer for
    anything over 8 bytes, ppc64le passes it inline.
  * true `...` varargs on the thunk itself.  We can forward a va_list we
    were handed, but we cannot synthesise one from an unknown argument
    count, so a variadic FunctionDecl is refused (see signature()).
  * float/vector/struct-by-value in the NAMED parameters, and any return
    wider than 64 bits.
"""

import json
import os
import subprocess
import tempfile

# __builtin_classify_type codes that MS-x64 passes in an integer register.
INTEGER_CLASSES = (1,   # integer_type_class
                   2,   # char_type_class
                   3,   # enumeral_type_class
                   4,   # boolean_type_class
                   5)   # pointer_type_class

# The translation unit the oracle reads.  windows.h alone is not enough: its
# include closure reaches only the CRT headers that corecrt.h happens to drag
# in (ctype/stddef/string/time/stdlib), so ucrtbase startup and stdio
# declarations were invisible and came back as "no declaration found in Wine
# headers" -- a tooling gap, not an ABI fact.  Every header below is Wine's
# own; adding them only widens what clang can see, it does not relax a single
# check.  They are listed explicitly rather than globbed so that what the
# oracle reads stays reviewable.
PROBE_SRC = (
    "#include <stdarg.h>\n"
    "#include <windows.h>\n"
    "#include <stdlib.h>\n"           # __p___argc, __p___wargv, exit
    "#include <stdio.h>\n"            # corecrt_stdio_config.h, corecrt_wstdio.h
    "#include <wchar.h>\n"            # __stdio_common_vswprintf
    "#include <string.h>\n"           # corecrt_wstring.h: wcsncmp
    "#include <process.h>\n"
    "#include <io.h>\n"               # corecrt_io.h: _setmode
    "#include <corecrt_startup.h>\n"  # _set_app_type, _configure_wide_argv, ...
)

# ---------------------------------------------------------------------------
# clang's PREDEFINED SUGAR TYPES are not spellable in source.
# ---------------------------------------------------------------------------
# Since clang 20, a function TYPE that mentions size_t / ptrdiff_t prints those
# with clang-internal canonical-sugar names -- `__size_t`, `__ptrdiff_t` -- and
# those identifiers do not exist for the preprocessor:
#
#     $ echo 'static __size_t a;' | clang -fsyntax-only -xc -
#     error: unknown type name '__size_t'; did you mean 'size_t'?
#
# ParmVarDecl nodes are unaffected (they print the source spelling, `size_t`),
# so only the RETURN spelling, which we split out of the function type, is hit.
# The symptom was every size_t-returning export -- strlen, wcslen, strcspn,
# fwrite -- coming back as
#     REFUSED: type specifier missing, defaults to 'int'
# which is a tooling artefact, not an ABI fact.
#
# Rewriting the internal name back to the source typedef is NOT a relaxation:
# the stage-2 probe still assigns the real function to the reconstructed
# prototype, so if `size_t` were not in fact the declared return type the
# reconstruction would still be a hard compile error.  Anything NOT in this map
# (`__signed_size_t`, and any future sugar kind) keeps failing exactly as it
# does today, and is refused with the compiler's own message.
SUGAR_SPELLING = {
    '__size_t':    'size_t',
    '__ptrdiff_t': 'ptrdiff_t',
}


# ---------------------------------------------------------------------------
# clang's NULLABILITY SPECIFIERS leak into the parameter spellings, sometimes
# TWICE, and are not valid source that way.
# ---------------------------------------------------------------------------
# When Wine's header declares a function clang also has a BUILTIN for, clang
# merges its builtin's attributes onto the explicit declaration.  Its builtin
# for vprintf is
#     int vprintf(const char *restrict, __builtin_va_list)
# with _Nonnull nullability on the first parameter, so Wine's plain
#     _ACRTIMP int __cdecl vprintf(const char*,va_list)   (msvcrt/stdio.h:711)
# comes back from the AST dump as
#     params: ['const char * _Nonnull _Nonnull', 'va_list']
# -- the specifier printed twice.  Splicing that into the stage-2 probe is
#     error: duplicate nullability specifier '_Nonnull' [-Wnullability]
# which looks like a refusal but is a printing artefact, not an ABI fact.
#
# Nullability specifiers are pure annotations.  They do not change a type's
# size, its __builtin_classify_type class, or how it is passed in any ABI --
# `const char * _Nonnull` and `const char *` are the same pointer in the same
# register.  Dropping them is therefore not a relaxation of any check, and the
# stage-2 probe still assigns the real function to the reconstructed prototype,
# so a genuinely wrong reconstruction is still a hard compile error.
#
# Deduplicating rather than deleting would also compile today, but the
# duplication is a clang printing quirk we should not be encoding a dependency
# on; the specifiers carry no information we use either way.
NULLABILITY = ('_Nonnull', '_Nullable', '_Null_unspecified', '_Nullable_result')


def _respell(spelling):
    """Make a clang type spelling valid source again.

    Two transformations, both provably ABI-inert -- see the SUGAR_SPELLING and
    NULLABILITY notes above:
      * clang's unspellable canonical-sugar names -> the source typedef
      * clang's nullability specifiers -> dropped
    """
    import re
    s = re.sub(r'\b(%s)\b' % '|'.join(SUGAR_SPELLING),
               lambda m: SUGAR_SPELLING[m.group(1)], spelling)
    s = re.sub(r'\s*\b(?:%s)\b' % '|'.join(NULLABILITY), '', s)
    return s.strip()


class Refused(Exception):
    def __init__(self, reason):
        Exception.__init__(self, reason)
        self.reason = reason


# ---------------------------------------------------------------------------
# WHICH C RUNTIME'S DECLARATIONS THE PROBE SEES  (load-bearing -- read this)
# ---------------------------------------------------------------------------
# Wine's include/msvcrt/ headers describe SEVERAL different C runtimes out of
# one source tree, selected by _MSVCR_VER / _UCRT.  include/msvcrt/corecrt.h:48
# defaults _MSVCR_VER to 140, and :52 turns that into _UCRT.  So with no define
# at all, the probe reads UCRTBASE's surface -- which is the wrong runtime for
# an msvcrt.dll thunk, and silently so:
#
#   * __getmainargs and __set_app_type are declared only under `#ifndef _UCRT`
#     (corecrt_startup.h:51,53+70).  In the default mode they are invisible and
#     came back as "no declaration found in Wine headers", pushing them onto
#     the weaker .spec fallback -- a tooling gap reported as an ABI fact.
#   * vprintf, _vsnprintf and friends have TWO declarations in stdio.h: a
#     `static inline` UCRT forwarder to __stdio_common_vfprintf (stdio.h:409),
#     and the real msvcrt import declaration (stdio.h:711).  In the default
#     mode the oracle read the INLINE -- i.e. not the DLL export at all.  The
#     arity happened to agree; nothing guaranteed it would.
#
# The rule below is Wine's own, lifted from tools/makedep.c:2456 get_crt_define():
# a module importing ucrt* is built with -D_UCRT, and anything else matching
# msvcr%u is built with -D_MSVCR_VER=<that number> -- "msvcrt" parses as
# version 0, so msvcrt.dll gets -D_MSVCR_VER=0.  Non-CRT modules (kernel32,
# ntdll, user32) get nothing, exactly as before this rule existed, so their
# existing specs are unaffected.
def crt_defines_for_dll(dll):
    """-> list of -D flags putting Wine's CRT headers in <dll>'s own mode.

    Mirrors tools/makedep.c:get_crt_define().  Returns [] for a module that is
    not a C runtime, which leaves clang's invocation byte-identical to what it
    was before this existed.
    """
    import re
    stem = os.path.splitext(os.path.basename(dll))[0].lower()
    if stem.startswith('ucrt'):
        return ['-D_UCRT']
    m = re.fullmatch(r'msvcr(\d*)', stem)
    if m:
        return ['-D_MSVCR_VER=%d' % int(m.group(1) or 0)]
    if stem == 'msvcrt':                      # sscanf("msvcrt", "msvcr%u") -> 0
        return ['-D_MSVCR_VER=0']
    return []


class WineHeaders:
    def __init__(self, include_dir, generated_dir=None, clang='clang',
                 target='x86_64-windows-gnu', workdir=None, defines=()):
        self.include_dir = os.path.abspath(os.path.expanduser(include_dir))
        self.generated_dir = (os.path.abspath(os.path.expanduser(generated_dir))
                              if generated_dir else None)
        self.defines = list(defines)
        self.clang = clang
        self.target = target
        self.workdir = workdir or tempfile.mkdtemp(prefix='winesig-')
        os.makedirs(self.workdir, exist_ok=True)
        self._probe = os.path.join(self.workdir, 'wt_probe.c')
        with open(self._probe, 'w') as f:
            f.write(PROBE_SRC)
        self._cache = {}

    # ---------------------------------------------------------------- clang

    def _incargs(self):
        a = []
        if self.generated_dir:
            a += ['-I' + self.generated_dir]
        a += ['-I' + self.include_dir, '-I' + os.path.join(self.include_dir, 'msvcrt')]
        return a

    def _base(self):
        return ([self.clang, '-target', self.target, '-fsyntax-only',
                 '-nostdlibinc'] + list(self.defines) + self._incargs())

    def sanity(self):
        """Prove the header set parses at all before trusting anything."""
        r = subprocess.run(self._base() + [self._probe],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("FATAL: Wine headers do not parse for %s:\n%s"
                             % (self.target, r.stderr[:4000]))

    # ------------------------------------------------- stage 1: AST structure

    @staticmethod
    def _split_json(text):
        dec, i, n, out = json.JSONDecoder(), 0, len(text), []
        while i < n:
            while i < n and text[i] in ' \t\r\n':
                i += 1
            if i >= n:
                break
            obj, i = dec.raw_decode(text, i)
            out.append(obj)
        return out

    def _ast(self, name):
        if name in self._cache:
            return self._cache[name]
        cmd = self._base() + ['-Xclang', '-ast-dump=json',
                              '-Xclang', '-ast-dump-filter=' + name, self._probe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        decls = []
        for obj in self._split_json(r.stdout):
            if obj.get('kind') != 'FunctionDecl' or obj.get('name') != name:
                continue
            # Skip clang's own implicit builtin declaration (wcsncmp, memcpy,
            # exit, ...).  It shadows Wine's header declaration and prints its
            # types with clang-internal names that are not spellable in source
            # -- `__size_t` -- so reconstructing from it produces an
            # implicit-int error that looks like a refusal but is not one.
            # We want what Wine's header says, so take only explicit decls.
            if obj.get('isImplicit'):
                continue
            ty = obj.get('type', {})
            qt = ty.get('qualType', '')
            # One level of desugaring strips a MacroQualifiedType, which clang
            # prints as the bare macro name: HeapAlloc's type comes back as
            # "__WINE_MALLOC LPVOID (HANDLE, DWORD, SIZE_T)", and splicing
            # that macro in front of a function-pointer declarator is an
            # ignored-attribute error.  desugaredQualType is the same type
            # without the macro sugar: "LPVOID (HANDLE, DWORD, SIZE_T)".
            rt = ty.get('desugaredQualType') or qt
            params = [c.get('type', {}).get('qualType')
                      for c in obj.get('inner', []) if c.get('kind') == 'ParmVarDecl']
            loc = obj.get('loc', {})
            # A macro-expanded declarator reports its spelling location; the
            # expansion location is the one a human can go and read.
            file_ = loc.get('file')
            line = loc.get('line')
            if file_ is None:
                beg = obj.get('range', {}).get('begin', {})
                exp = beg.get('expansionLoc', beg)
                file_, line = exp.get('file'), exp.get('line')
            decls.append(dict(file=file_, line=line, qualtype=qt, rettype=rt,
                              params=params,
                              variadic=qt.rstrip().endswith('...)')
                              or ', ...)' in qt))
        self._cache[name] = decls
        return decls

    @staticmethod
    def _return_spelling(qualtype):
        """Everything left of the parameter list in a clang function qualType."""
        depth = 0
        for i, ch in enumerate(qualtype):
            if ch == '(':
                if depth == 0:
                    return qualtype[:i].strip()
                depth += 1
            elif ch == ')':
                depth -= 1
        raise Refused('cannot split return type out of %r' % qualtype)

    # ---------------------------------------- stage 2: compiler-checked probe

    def _probe_source(self, name, ret, params):
        L = [PROBE_SRC]
        arglist = ', '.join(params) if params else 'void'
        # If this reconstruction is not the real prototype, this is a hard error.
        L.append('static %s (*wt_reconstructed)(%s) = &%s;\n' % (ret, arglist, name))
        L.append('void wt_use(void) { (void)wt_reconstructed; }\n')

        def intcheck(tag, spelling):
            ors = ' || '.join('__builtin_classify_type(*(%s *)0) == %d'
                              % (spelling, c) for c in INTEGER_CLASSES)
            L.append('_Static_assert(%s, "%s: %s is not integer/pointer class");\n'
                     % (ors, name, tag))
            L.append('_Static_assert(sizeof(%s) <= 8, "%s: %s is wider than 64 bits");\n'
                     % (spelling, name, tag))

        for i, p in enumerate(params):
            intcheck('argument %d (%s)' % (i, p), p)
        if ret.strip() != 'void':
            intcheck('return value (%s)' % ret, ret)
        return ''.join(L)

    def _compile_probe(self, name, src):
        p = os.path.join(self.workdir, 'wt_check_%s.c' % name)
        with open(p, 'w') as f:
            f.write(src)
        r = subprocess.run(self._base() + ['-Werror', p],
                           capture_output=True, text=True)
        return r.returncode, r.stderr

    @staticmethod
    def _first_reason(stderr):
        """Pull our own _Static_assert text out of clang's diagnostic."""
        for ln in stderr.splitlines():
            if 'error:' in ln:
                msg = ln.split('error:', 1)[1].strip()
                # "static assertion failed due to requirement '<expr>': <ours>"
                if "': " in msg:
                    msg = msg.rsplit("': ", 1)[1]
                return msg.strip('"')
        return stderr.strip()[:200] or 'probe failed to compile'

    # ------------------------------------------------------------------ API

    def signature(self, name):
        """-> dict(nargs, returns_void, header, line, ret, params).

        Raises Refused with a human reason for anything not representable as
        "N integer/pointer arguments, one <=64-bit integer/pointer or void
        return".  Never guesses.
        """
        decls = self._ast(name)
        if not decls:
            raise Refused('no declaration found in Wine headers')

        shapes = {(d['qualtype'],) for d in decls}
        if len(shapes) != 1:
            raise Refused('conflicting declarations: %s'
                          % '; '.join(sorted(s[0] for s in shapes)))
        d = decls[0]
        if d['variadic']:
            raise Refused('varargs (%s)' % d['qualtype'])
        if any(p is None for p in d['params']):
            raise Refused('unreadable parameter type in %s' % d['qualtype'])

        ret = _respell(self._return_spelling(d.get('rettype') or d['qualtype']))
        params = [_respell(p) for p in d['params']]
        if len(params) > 16:
            raise Refused('%d arguments, descriptor field holds at most 16'
                          % len(params))

        rc, err = self._compile_probe(name, self._probe_source(name, ret, params))
        if rc != 0:
            raise Refused(self._first_reason(err))

        hdr = d['file'] or '<unknown>'
        return dict(nargs=len(params),
                    returns_void=(ret.strip() == 'void'),
                    header=os.path.basename(hdr),
                    header_path=hdr,
                    line=d['line'],
                    ret=ret,
                    params=params,
                    qualtype=d['qualtype'])


class WineSpecs:
    """LAST-RESORT arity oracle for exports Wine declares in NO header.

    Read the warning before using this.  A Wine .spec line is a much weaker
    source than a header and is only usable because our descriptor is so
    narrow:

      * It carries NO return type at all -- not even void vs non-void.  So
        the <int|void> column of an entry resolved this way is an UNCHECKED
        HUMAN ASSERTION, and every such entry is printed under a loud banner.
        Take it from the C definition in Wine's source and cite that line in
        a comment above the entry.
      * Its `long` is 32 bits, which is exactly what burned the sibling
        project wine-spec-thunk.  That does not hurt US: our descriptor
        encodes only an argument COUNT, and a 32-bit long and a 64-bit
        pointer each occupy exactly one 8-byte slot.  Width is irrelevant
        here; it would not be for a marshalling thunk.
      * What it does state exactly is the arity and the CLASS of each
        argument, so a float/double/int128 argument is still detectable and
        is still refused below.

    This path exists for kernel32's delay-load entry points, which Wine
    implements in C and exports from a .spec but declares in no header at
    all.  Anything that HAS a header declaration must go through WineHeaders
    instead -- the header oracle is the default and this is the exception.
    """

    # Wine .spec argument tokens that occupy exactly one integer/pointer slot.
    SLOT_ARGS = {'word', 'long', 'int64', 'ptr', 'str', 'wstr', 'segptr',
                 'segstr', 'handle'}
    # ...and the ones that do not, with why.
    BAD_ARGS = {'double': 'double argument (SSE register on MS-x64, FPR on '
                          'ppc64le -- not a plain slot)',
                'float':  'float argument (SSE register on MS-x64, FPR on '
                          'ppc64le -- not a plain slot)',
                'int128': 'int128 argument (wider than one 64-bit slot)'}

    def __init__(self, source_root):
        self.root = os.path.abspath(os.path.expanduser(source_root))

    def _find(self, specname):
        direct = os.path.join(self.root, specname)
        if os.path.isfile(direct):
            return direct
        import glob
        hits = glob.glob(os.path.join(self.root, 'dlls', '*', specname))
        if len(hits) == 1:
            return hits[0]
        if not hits:
            raise Refused('no %s under %s/dlls' % (specname, self.root))
        raise Refused('ambiguous %s: %s' % (specname, ', '.join(sorted(hits))))

    def signature(self, name, specname, lineno, asserted_returns_void):
        path = self._find(specname)
        with open(path) as f:
            lines = f.readlines()
        if not (1 <= lineno <= len(lines)):
            raise Refused('%s has no line %d' % (specname, lineno))
        raw = lines[lineno - 1].split('#', 1)[0].strip()
        if not raw.startswith('@'):
            raise Refused('%s:%d is not an export line: %r'
                          % (specname, lineno, raw[:80]))
        if '(' not in raw or ')' not in raw:
            raise Refused('%s:%d has no argument list: %r'
                          % (specname, lineno, raw[:80]))
        head, _, rest = raw.partition('(')
        arglist, _, _tail = rest.rpartition(')')
        # "@ stdcall -import DelayLoadFailureHook" -> the name is the last token
        decl_name = head.split()[-1]
        if decl_name != name:
            raise Refused('%s:%d declares %r, not %r'
                          % (specname, lineno, decl_name, name))
        if ' -arch=' in head or head.endswith('-arch'):
            raise Refused('%s:%d is architecture-conditional (-arch=), so the '
                          'line cited may not be the one that applies'
                          % (specname, lineno))
        args = arglist.split()
        for a in args:
            if a in self.BAD_ARGS:
                raise Refused(self.BAD_ARGS[a])
            if a not in self.SLOT_ARGS:
                raise Refused('unknown .spec argument type %r at %s:%d'
                              % (a, specname, lineno))
        if len(args) > 16:
            raise Refused('%d arguments, descriptor field holds at most 16'
                          % len(args))
        return dict(nargs=len(args),
                    returns_void=asserted_returns_void,
                    return_verified=False,
                    header=specname,
                    header_path=path,
                    line=lineno,
                    ret='<unverified: .spec carries no return type>',
                    params=args,
                    qualtype='%s(%s)  [from %s:%d]'
                             % (name, ' '.join(args), specname, lineno))


def descriptor(sig):
    """Pack a signature into the uint32 that sigs_rva[i] holds."""
    if not (0 <= sig['nargs'] <= 16):
        raise Refused('nargs out of range')
    return (sig['nargs'] & 0xff) | (0x100 if sig['returns_void'] else 0)


# This is a build tool living at <wine-root>/tools/spec2thunk/, so the source
# tree is two directories up.  Nothing here may assume a particular checkout
# location -- it used to, and that is exactly what made it scaffolding.
_TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
WINE_ROOT = os.path.dirname(os.path.dirname(_TOOL_DIR))

DEFAULT_SOURCE = WINE_ROOT
DEFAULT_INCLUDE = os.path.join(WINE_ROOT, 'include')
def find_generated_include():
    """Locate the widl-generated headers (wtypes.h and friends).

    They are BUILD output, not source, so they are not under include/ in the
    source tree.  Wine tools are invoked from the top of the build directory,
    so that is the first place to look; an in-tree build also has them beside
    the source headers.  Returns None when neither has them, so the caller can
    say plainly that --wine-generated is needed rather than failing later with
    a confusing 'wtypes.h: No such file' out of clang.
    """
    for cand in (os.path.join(os.getcwd(), 'include'), DEFAULT_INCLUDE):
        if os.path.exists(os.path.join(cand, 'wtypes.h')):
            return cand
    return None


if __name__ == '__main__':
    import sys
    argv = sys.argv[1:]
    # `wine_sig.py --dll=msvcrt.dll vprintf ...` puts the CRT headers in that
    # module's own mode, the same way spec2thunk does from the DLL line of
    # a .thunks file.  Without it you get the default (UCRT) view.
    defines = []
    while argv and argv[0].startswith('--dll='):
        defines = crt_defines_for_dll(argv.pop(0).split('=', 1)[1])
    if defines:
        print('# CRT mode: %s' % ' '.join(defines))
    w = WineHeaders(DEFAULT_INCLUDE, DEFAULT_GENERATED,
                    workdir='/tmp/imports-work/build/sigprobe',
                    defines=defines)
    w.sanity()
    for n in argv:
        try:
            s = w.signature(n)
            print('%-26s nargs=%d void=%d desc=0x%03x  %s:%s  %s'
                  % (n, s['nargs'], s['returns_void'], descriptor(s),
                     s['header'], s['line'], s['qualtype']))
        except Refused as e:
            print('%-26s REFUSED: %s' % (n, e.reason))
