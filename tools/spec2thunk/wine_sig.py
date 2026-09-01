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

---------------------------------------------------------------------------
TRUE `...` VARIADICS ARE SUPPORTED, BY FORWARDING TO THE CALLEE'S OWN
v-VARIANT.  OPT-IN ONLY.  READ THIS BEFORE "FIXING" IT BACK.
---------------------------------------------------------------------------
The obvious objection is correct as far as it goes: a FIXED-ARITY stub cannot
forward `...`, because it cannot know how many arguments the guest pushed.
The trick is that it does not have to.  It never calls the variadic function
at all.  It calls that function's OWN v-variant --

    printf     -> vprintf          (1 fixed arg  -> 2 args)
    _snprintf  -> _vsnprintf       (3 fixed args -> 4 args)

-- handing it a va_list that the HOST synthesises from the guest's argument
frame.  The v-variant is an ordinary non-variadic function of nfixed+1
arguments, which this file has always been able to describe.

That works only because of a measured property of THIS abi pair, and would
not transfer to another one:

  * MS-x64 homes RCX/RDX/R8/R9 into the caller's 32-byte shadow space, so
    from the first vararg onward the guest's arguments are a flat run of
    8-byte slots, and a `char *` pointing at the first of them IS an MS-x64
    va_list.
  * ppc64le ELFv2 va_list is likewise a plain pointer into a flat 8-byte-slot
    parameter save area -- no register-save-area half to reconstruct, unlike
    SysV x86-64, where this would be impossible without rebuilding a
    __va_list_tag.

Measured end to end on the AC922: a guest-built MS-x64 va_list was consumed
correctly by native ppc64 `_vsnprintf`, `"%d-%s-%d"` -> `"4242-AB-77"`.

The sibling project (wine-spec-thunk, gen_spec_thunk.py) refuses va_list
outright.  That refusal was written for a different boundary; it is not an
ABI fact about this one.  Do not copy it back here.

The opt-in is deliberate and belongs to the SPEC, not to this file: a
variadic export is only described when its .thunks line names the v-variant
explicitly (`variadic=vprintf`), and the caller then passes
allow_variadic=True.  A variadic FunctionDecl with no such opt-in is still
refused exactly as it always was, because nothing has told us what to call
instead.  Nothing here derives the v-variant name by string munging -- the
spec supplies it and the oracle is used to CONFIRM it (nargs(impl) must be
nfixed+1); see resolve_signatures() in spec2thunk.

Still genuinely unsupported, still refused:
  * a `long double` vararg -- 128-bit on ppc64, 64-bit on MS-x64, so the
    slot widths differ and the walk desynchronises.
  * a struct passed BY VALUE as a vararg -- MS-x64 passes a pointer for
    anything over 8 bytes, ppc64le passes it inline.
  * a variadic with ZERO fixed arguments -- there is no anchor to start the
    walk from, and no such thing exists in a C runtime's surface anyway.
  * float/vector/struct-by-value in the NAMED parameters, and any return
    wider than 64 bits.

---------------------------------------------------------------------------
BATCHING.  ONE clang PER MODULE, NOT ONE PER SYMBOL.  MUST NOT CHANGE ANSWERS.
---------------------------------------------------------------------------
Both stages used to run once per export, which is fine for a hand-written list
of 150 names and hopeless for a whole .spec (kernel32 has 1342 non-stub
exports).  Measured on the AC922: one `-ast-dump-filter=<name>` dump is 0.58 s,
one FULL unfiltered dump of the same probe is 1.49 s and contains every one of
the 6284 visible FunctionDecls.  So the filtered dump is not a cheaper query,
it is the same work done 1342 times.

  stage 1 is now dumped ONCE, unfiltered, and indexed by name.  A filtered
  dump and an unfiltered one traverse the same list -- clang's ASTPrinter walks
  the translation unit's own decls either way, and the filter only decides
  which of them get printed -- so the FunctionDecl nodes are the same nodes.
  _decl_from_node() is the single reader of a node and is shared by both paths,
  so an indexed answer cannot drift from a filtered one.

  stage 2 is COMPILED ONCE for the whole module, as one translation unit made
  of per-export chunks that share nothing but the headers.  This is a fast path
  for the ACCEPT case only: if the batch compiles clean, every chunk in it
  compiled clean, and each export is memoised accepted.  If it does not, every
  export whose own lines a diagnostic points at -- and, if a diagnostic cannot
  be attributed at all, every export in the batch -- falls back to its OWN
  single-symbol probe, so a REFUSAL is still produced by exactly the code and
  the exact source text that produced it before, and carries the same message.
  That is why the per-symbol probe is kept rather than deleted: it is the
  authority on refusals, and refusals are the minority.

Verified answer-for-answer: regenerating kernel32, msvcrt, ntdll, ucrtbase and
user32 with batching on and off produces byte-identical DLLs.  set batch=False
(or SPEC2THUNK_NO_BATCH=1) to re-run that comparison.
"""

import json
import os
import re
import subprocess
import tempfile

# __builtin_classify_type codes that MS-x64 passes in an integer register.
INTEGER_CLASSES = (1,   # integer_type_class
                   2,   # char_type_class
                   3,   # enumeral_type_class
                   4,   # boolean_type_class
                   5)   # pointer_type_class

RECORD_CLASS = 12       # record_type_class  (struct)
UNION_CLASS  = 13       # union_type_class
REAL_CLASS   = 8        # real_type_class  (float, double, long double)

# Floating point IS representable across this ABI pair, in the first EIGHT
# argument positions.
#
# The bound is the descriptor's, not the ABI's.  MS-x64 has exactly XMM0-XMM3
# for arguments and passes a fifth floating-point argument on the STACK, in
# the same eight-byte slot an integer would have used -- so the host can see
# it perfectly well, and marshal_thunk_args_fp() in dlls/ntdll/signal_ppc64.c
# reads position i from XMMi below four and from [Rsp + 8 + 8*i] at or above
# it.  What stops at eight is THUNK_FP_MASK: the FP descriptor word carries
# one bit per argument in a byte, and ELFv2's f1-f8 are what
# call_native_thunk_fp loads.
#
# This used to be 4, describing the host side as it then was -- it read every
# FP argument out of an XMM register, so a fifth one would have come from
# XMM4, which is not an argument register at all.  That refused glOrtho,
# glFrustum, glUniform4f, glMultiTexCoord4f and 155 other OpenGL entry points
# whose fourth-and-later arguments are floats, which is how the gap surfaced.
# Do not lower it again without lowering the host side with it: a descriptor
# that promises less than the marshaller does costs nothing, but one that
# promises more is a wrong number rather than a crash.
#
# A `long double` is refused whatever its position: it is 128-bit on ppc64 and
# 64-bit on MS-x64, so the value would not survive.  sizeof <= 8 in the probe
# below is what enforces that.
MAX_FP_ARG_POS = 8

# ---------------------------------------------------------------------------
# SMALL AGGREGATES PASSED BY VALUE.  ONE GPR IN BOTH ABIS.  NAMED, NOT INFERRED.
# ---------------------------------------------------------------------------
# MS-x64 passes an aggregate of 1, 2, 4 or 8 bytes BY VALUE in one integer
# register (anything else goes by reference).  ppc64le ELFv2 places an
# aggregate of <= 8 bytes in one GPR, its bytes in memory order, which on
# little-endian is the same eight bytes in the same register.  The trap stub
# already captures the guest's slot as a 64-bit value and the host passes it as
# a ULONG_PTR, landing in r3 -- exactly where ELFv2 wants the aggregate -- so
# nothing in the generated code changes.  This is purely what the oracle will
# ACCEPT.
#
# The exception, and the reason this is a named set rather than a size rule:
# ELFv2 passes a HOMOGENEOUS FLOAT AGGREGATE in FPRs, while MS-x64 keeps it in
# a GPR.  struct { float x, y; } is 8 bytes and would sail through a pure size
# test while landing in the wrong register file.  Proving "no floating-point
# member anywhere, recursively" needs the record's own definition, and stage 1
# indexes FunctionDecls only -- so until it also indexes RecordDecls, each type
# here is one somebody read.  Adding to this list means reading the struct and
# writing down what is in it.
#
#   POINT           struct { LONG x, y; }                    8 bytes, 2x int32
#   POINTS          struct { SHORT x, y; }                   4 bytes, 2x int16
#   SIZE            struct { LONG cx, cy; }                  8 bytes, 2x int32
#   LARGE_INTEGER   union of { DWORD,LONG } and LONGLONG     8 bytes, integer
#   ULARGE_INTEGER  union of { DWORD,DWORD } and ULONGLONG   8 bytes, integer
#   ldiv_t          struct { __msvcrt_long quot, rem; }      8 bytes, 2x int32
#                   (include/msvcrt/stdlib.h; __msvcrt_long is always 32-bit,
#                   unlike the host's `long`, so this is 8 bytes on every
#                   build host this oracle runs on).  msvcrt/ucrtbase's ldiv
#                   RETURNS this by value (lldiv_t's two __int64 members make
#                   it 16 bytes, over the limit, and it is correctly refused
#                   without help).
#   CY              union of { struct{ULONG,LONG}, LONGLONG } (include/
#                   wtypes.idl) -- 8 bytes, integer both ways.  oleaut32's
#                   whole Var*FromCy / VarCyFrom* / VarCy{Add,Mul,Abs,...}
#                   family takes or returns this BY VALUE.
#
# The generated probe still asserts sizeof <= 8 AND that the type really is a
# struct or union, so a type that changes shape under this list fails the build
# rather than silently passing in the wrong register file.
AGGREGATE_SLOT_TYPES = frozenset((
    'POINT', 'POINTS', 'SIZE', 'LARGE_INTEGER', 'ULARGE_INTEGER',
    'ldiv_t', 'CY',
))


def _bare_type(spelling):
    """Strip cv-qualifiers so a whitelist lookup sees the type itself."""
    t = spelling.strip()
    for q in ('const ', 'volatile '):
        while t.startswith(q):
            t = t[len(q):].strip()
    return t

# The translation unit the oracle reads.  windows.h alone is not enough: its
# include closure reaches only the CRT headers that corecrt.h happens to drag
# in (ctype/stddef/string/time/stdlib), so ucrtbase startup and stdio
# declarations were invisible and came back as "no declaration found in Wine
# headers" -- a tooling gap, not an ABI fact.  Every header below is Wine's
# own; adding them only widens what clang can see, it does not relax a single
# check.  They are listed explicitly rather than globbed so that what the
# oracle reads stays reviewable.
#
# The second group below is Win32 headers that windows.h does NOT pull in --
# it stops at winbase/wingdi/winuser/winnls/wincon/winver/winreg/winnetwk and
# the OLE set (see include/windows.h).  Wine's own kernel32 sources include
# them by name, so the declarations exist and only the PROBE could not see
# them.  MEASURED on a whole-kernel32.spec run: 39 exports came back "no
# declaration found in Wine headers" purely because of this, out of 179 -- a
# tooling gap being reported as an ABI fact, which is the one thing this file
# must never do.  Each line below says which exports it is there for.
PROBE_SRC = (
    "#include <stdarg.h>\n"
    # BEFORE windows.h, and this order is load-bearing: windows.h pulls in the
    # winsock v1 winsock.h, and ws2def.h then redefines struct sockaddr with a
    # different definition.  Gets ws2_32's inet_pton/inet_ntop/getaddrinfo and
    # the WSA* surface that winsock.h alone does not declare.
    "#include <winsock2.h>\n"
    "#include <ws2tcpip.h>\n"
    "#include <windows.h>\n"
    "#include <stdlib.h>\n"           # __p___argc, __p___wargv, exit
    "#include <stdio.h>\n"            # corecrt_stdio_config.h, corecrt_wstdio.h
    "#include <wchar.h>\n"            # __stdio_common_vswprintf
    "#include <string.h>\n"           # corecrt_wstring.h: wcsncmp
    "#include <process.h>\n"
    "#include <io.h>\n"               # corecrt_io.h: _setmode
    "#include <corecrt_startup.h>\n"  # _set_app_type, _configure_wide_argv, ...
    "#include <fileapi.h>\n"          # CreateFile2, Find{First,Next}FileNameW,
                                      # GetTempPath2A/W
    "#include <tlhelp32.h>\n"         # CreateToolhelp32Snapshot, Module32*,
                                      # Process32*, Thread32*, Heap32*
    "#include <werapi.h>\n"           # WerRegisterFile, WerSetFlags, ...
    "#include <appmodel.h>\n"         # AppPolicyGet*
    "#include <processsnapshot.h>\n"  # PssCaptureSnapshot, PssFreeSnapshot, ...
    "#include <winternl.h>\n"         # the NT surface kernel32 re-exports
    "#include <patchapi.h>\n"         # the whole mspatcha surface
    "#include <appcompatapi.h>\n"     # apphelp: ApphelpCheckShellObject, ...
    "#include <aclapi.h>\n"           # advapi32: SetEntriesInAcl*, BuildSecurityDescriptor*,
                                      # {Get,Set}{Named,}SecurityInfo*, GetExplicitEntriesFromAcl*
    "#include <sddl.h>\n"             # advapi32: ConvertSid<->StringSid*,
                                      # Convert{,String}SecurityDescriptor*
    "#include <wmistr.h>\n"           # WNODE_HEADER etc., required BEFORE evntrace.h
    "#include <evntrace.h>\n"         # advapi32: StartTrace*, ControlTrace*, EnableTrace*
    "#include <perflib.h>\n"          # advapi32: PerfStartProvider*, PerfStopProvider, ...
    "#include <ntsecapi.h>\n"         # advapi32: LsaOpenPolicy, LsaClose, LsaLookup*,
                                      # LsaQueryInformationPolicy, ...
    "#include <threadpoolapiset.h>\n" # kernel32/kernelbase: CallbackMayRunLong
    "#include <winhttp.h>\n"          # winhttp: the whole WinHttp* surface, which
                                      # PlayFabMultiplayerWin.dll imports
    "#include <conio.h>\n"            # msvcrt/ucrtbase: _getch, _kbhit, _cputs
    "#include <direct.h>\n"           # msvcrt/ucrtbase: _getcwd, _chdir, _mkdir
    "#include <assert.h>\n"           # msvcrt/ucrtbase: _wassert
    "#include <math.h>\n"             # msvcrt/ucrtbase: the whole math surface
    "#include <signal.h>\n"           # msvcrt/ucrtbase: signal, raise
    "#include <shlobj.h>\n"           # shell32: SHGetKnownFolderPath, SHGetFolderPath*
    "#include <evntprov.h>\n"         # advapi32: EventRegister, EventWriteTransfer,
                                      # EventSetInformation

    # The 2026-08-15 first-real-game set.  Each of these modules refused 100% of
    # its exports as "no declaration found in Wine headers" -- the tooling gap
    # this list exists to close, not an ABI fact about the module.
    "#include <avrt.h>\n"             # avrt: AvSetMmThreadCharacteristics*,
                                      # AvRevertMmThreadCharacteristics, ...
    "#include <setupapi.h>\n"         # setupapi: the SetupDi* and SetupQueue* surface
    "#include <cfgmgr32.h>\n"         # setupapi: the CM_* / CMP_* half, which
                                      # setupapi.h does not declare
    "#include <d3dcompiler.h>\n"      # d3dcompiler_47: D3DCompile*, D3DReflect, ...
    "#include <dxgi1_6.h>\n"          # dxgi: CreateDXGIFactory{,1,2}; 1_6 pulls the
                                      # whole 1_0..1_6 chain
    "#include <d3d11_4.h>\n"          # d3d11: D3D11CreateDevice{,AndSwapChain}
    "#include <ddk/d3dkmthk.h>\n"     # d3d11 re-exports the D3DKMT* thunks
    # The one member of the D3D family this list still had to be told about
    # module by module.  d3d9.thunks reaches it through DXVK's own
    # dxvk_flat_surface_d3d9.h and dxva2.thunks names it outright as its first
    # PROBE-EXTRA -- both because dxva2api.h will not even compile without
    # D3DFORMAT and D3DPOOL, which live in the d3d9types.h that d3d9.h pulls
    # in.  Two modules working around the same absent header by hand is the
    # shape of a gap in THIS list, not of two module-specific facts, and a
    # third module would have found it the same way: as 100% of its exports
    # refused for "no declaration found in Wine headers".
    "#include <d3d9.h>\n"             # d3d9: Direct3DCreate9{,Ex}, D3DPERF_*;
                                      # dxva2: DXVA2CreateDirect3DDeviceManager9
                                      # and the IDirect3DDevice9 half of its
                                      # surface
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


# ---------------------------------------------------------------------------
# clang PRINTS A CALLING CONVENTION WHERE IT CANNOT BE WRITTEN.
# ---------------------------------------------------------------------------
# A function-POINTER parameter comes back with the pointee's convention printed
# after the declarator, which is not a position C accepts:
#
#     qsort -> params[3] = 'int (*)(const void *, const void *)
#                           __attribute__((cdecl))'
#
# Splicing that into the stage-2 probe is
#     error: use of undeclared identifier '__cdecl__'
# (Wine's headers #define cdecl, so the attribute name macro-expands and stops
# being an attribute at all).  That is a printing artefact exactly like the
# `__size_t` sugar and the doubled nullability specifiers above, and it was
# refusing qsort, bsearch, qsort_s and atexit from every CRT module -- four
# exports a real guest binary imports.
#
# DROPPING IT IS ABI-INERT AND BACKSTOPPED, TWICE OVER.  A function pointer is
# one 8-byte pointer in one integer register whatever the pointee's convention
# is, so the descriptor cannot change.  And the reconstruction is still
# ASSIGNED the real function: if the convention actually mattered the two
# pointer types would be incompatible and the probe would fail with clang's own
# "incompatible pointer types" -- a refusal, not a wrong answer.  Only the
# convention attributes are dropped; every other attribute keeps failing
# exactly as it does today.
CONV_ATTR = re.compile(
    r'\s*__attribute__\(\(\s*(?:__)?'
    r'(?:cdecl|stdcall|fastcall|thiscall|vectorcall|regcall|ms_abi|sysv_abi)'
    r'(?:__)?\s*\)\)')


# ---------------------------------------------------------------------------
# "POINTER TO <T>" IS NOT "<T> *" WHEN <T> IS AN ABSTRACT DECLARATOR.
# ---------------------------------------------------------------------------
# The stage-2 probe needs an expression of each parameter's type and builds one
# as `*(<T> *)0`.  Appending ` *` is correct for `const char *` and for `DWORD`
# and wrong for every declarator with inner parentheses -- a function pointer:
#
#     int (*)(const void *, const void *) *      <- error: expected ')'
#
# The pointer has to go INSIDE the declarator: `int (**)(const void *, const
# void *)`.  This is the second half of what was refusing qsort, bsearch,
# qsort_s and atexit; fixing only the printed calling convention just moved the
# error from "undeclared identifier '__cdecl__'" to "expected ')'".
#
# Inserting one `*` in the innermost declarator is the C spelling of "pointer
# to that type" -- it is not a relaxation, and `sizeof` still uses the type
# UNCHANGED.  The outermost `(*` is the first one in the string, because the
# return type is printed before it, so a parameter that is itself a pointer to
# a function taking a function pointer still gets the outer declarator.
_PTR_DECL = re.compile(r'\(\s*\*')


def _ptr_to(spelling):
    """-> a valid spelling of "pointer to <spelling>"."""
    m = _PTR_DECL.search(spelling)
    if m:
        return spelling[:m.end()] + '*' + spelling[m.end():]
    return spelling + ' *'


def _respell(spelling):
    """Make a clang type spelling valid source again.

    Three transformations, all provably ABI-inert -- see the SUGAR_SPELLING,
    NULLABILITY and CONV_ATTR notes above:
      * clang's unspellable canonical-sugar names -> the source typedef
      * clang's nullability specifiers -> dropped
      * a calling convention printed after a declarator -> dropped
    """
    s = re.sub(r'\b(%s)\b' % '|'.join(SUGAR_SPELLING),
               lambda m: SUGAR_SPELLING[m.group(1)], spelling)
    s = re.sub(r'\s*\b(?:%s)\b' % '|'.join(NULLABILITY), '', s)
    s = CONV_ATTR.sub('', s)
    return s.strip()


class Refused(Exception):
    def __init__(self, reason):
        Exception.__init__(self, reason)
        self.reason = reason


def _split_top(text, sep=','):
    """Split on `sep` at parenthesis/bracket depth 0 only."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


def _toplevel_params(functype):
    """-> list of top-level parameter spellings of a clang function type.

    `functype` is a whole function type as clang prints it, e.g.
    'int (char *, size_t, const char *, ...)'.  Returns the contents of the
    OUTERMOST parameter list, split at depth 0.

    Naive substring tests on the printed type get this wrong: a parameter that
    is itself a pointer to a variadic function -- 'int (int (*)(char *, ...))'
    -- contains ', ...)' while the function itself is NOT variadic.  Mistaking
    that for a variadic would be a silent arity error, so the parameter list is
    parsed rather than pattern-matched.
    """
    depth, start = 0, None
    for i, ch in enumerate(functype):
        if ch == '(':
            if depth == 0 and start is None:
                start = i + 1
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0 and start is not None:
                return _split_top(functype[start:i])
    raise Refused('cannot find a parameter list in %r' % functype)


def _is_variadic(functype):
    """True iff the OUTERMOST parameter list ends in a literal '...'."""
    try:
        params = _toplevel_params(functype)
    except Refused:
        return False
    return bool(params) and params[-1] == '...'


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
                 target='x86_64-windows-gnu', workdir=None, defines=(),
                 batch=None, probe_src=None, probe_extra='', extra_includes=()):
        """probe_src: the translation unit the oracle reads, when the module's
        declarations do not live in Wine's headers at all.  The d3d12 thunk is
        the motivating case: its surface is vkd3d-proton's own widl output
        (PROBE-INCLUDE lines in the .thunks file), never Wine's d3d12.h --
        thunking a graphics module toward Wine's implementation is the one
        thing the generator must make unrepresentable.  extra_includes are -I
        directories searched BEFORE Wine's include tree for the same reason.
        Neither relaxes a check: the reconstruction probe still compiles
        against whatever TU is named here."""
        self.include_dir = os.path.abspath(os.path.expanduser(include_dir))
        self.generated_dir = (os.path.abspath(os.path.expanduser(generated_dir))
                              if generated_dir else None)
        self.defines = list(defines)
        self.extra_includes = [os.path.abspath(os.path.expanduser(d))
                               for d in extra_includes]
        self.clang = clang
        self.target = target
        self.workdir = workdir or tempfile.mkdtemp(prefix='winesig-')
        os.makedirs(self.workdir, exist_ok=True)
        # probe_extra APPENDS; probe_src REPLACES.  A module can need both:
        # the Wine-centric default plus a private header of its own.
        self.probe_src = (probe_src if probe_src is not None else PROBE_SRC) + probe_extra
        self._probe = os.path.join(self.workdir, 'wt_probe.c')
        with open(self._probe, 'w') as f:
            f.write(self.probe_src)
        self._cache = {}
        # name -> [decl, ...] for the WHOLE header surface, built on first use.
        # None until then; see _build_index() and the BATCHING banner above.
        self._index = None
        # (name, ret, params, variadic) -> (returncode, stderr) for a stage-2
        # probe already decided, whether one at a time or inside a batch.
        self._probe_memo = {}
        if batch is None:
            batch = os.environ.get('SPEC2THUNK_NO_BATCH', '') in ('', '0')
        self.batch = bool(batch)
        # Counters, so a caller can SAY what it did rather than assume it.
        self.stats = dict(ast_dumps=0, probe_compiles=0, batched_probes=0,
                          fallback_probes=0, indexed_decls=0)

    # ---------------------------------------------------------------- clang

    def _incargs(self):
        a = ['-I' + d for d in self.extra_includes]
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

    @staticmethod
    def _decl_from_node(obj, name):
        """-> the one decl dict we keep for a FunctionDecl node, or None.

        THE SINGLE READER OF A CLANG AST NODE.  Both the per-symbol filtered
        dump and the whole-translation-unit index go through this, so an
        indexed answer cannot drift from a filtered one -- there is only one
        piece of code that turns a node into an answer.
        """
        if obj.get('kind') != 'FunctionDecl' or obj.get('name') != name:
            return None
        # Skip clang's own implicit builtin declaration (wcsncmp, memcpy,
        # exit, ...).  It shadows Wine's header declaration and prints its
        # types with clang-internal names that are not spellable in source
        # -- `__size_t` -- so reconstructing from it produces an
        # implicit-int error that looks like a refusal but is not one.
        # We want what Wine's header says, so take only explicit decls.
        if obj.get('isImplicit'):
            return None
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
        return dict(file=file_, line=line, qualtype=qt, rettype=rt,
                    params=params, variadic=_is_variadic(rt))

    # -----------------------------------------------------------------------
    # clang's JSON AST DUMP OMITS A LOCATION'S `file` WHEN IT HAS NOT CHANGED.
    # -----------------------------------------------------------------------
    # JSONNodeDumper::writeBareSourceLocation prints `file` only when the
    # buffer name differs from the previously printed location's, and `line`
    # only when the line number differs.  Per-symbol `-ast-dump-filter` runs
    # hid this: the FIRST location a run prints always carries its file, and a
    # filtered run prints almost nothing else.  In ONE unfiltered dump only 17
    # of 5868 FunctionDecls carry a `file` at all, and the other 5851 came back
    # as "<unknown>:None" -- which spec2thunk correctly rejected as a
    # spec/header disagreement rather than emitting a wrong citation.
    #
    # `includedFrom` is NOT the answer: it names the file doing the #include,
    # not the file the declaration is in.
    #
    # The state is reconstructed by replaying the omission rule in the order
    # clang emitted the locations.  json.loads preserves object key order, and
    # a pre-order walk of the parsed tree in key order therefore visits every
    # location in exactly its serialisation order -- which IS clang's emission
    # order.  The walk is generic (any object carrying an `offset` is a bare
    # location) so it cannot be desynchronised by a location attached to some
    # node kind this file does not know about.
    @staticmethod
    def _fill_omitted_locations(tu):
        """Restore the `file` (and `line`) clang left out.  In place."""
        cur_file, cur_line = None, None
        stack = [tu]
        while stack:
            o = stack.pop()
            if type(o) is dict:
                if 'offset' in o:               # a bare source location
                    f = o.get('file')
                    if f is not None:
                        # file printed => line printed too, always.
                        cur_file, cur_line = f, o.get('line', cur_line)
                    elif 'line' in o:
                        o['file'], cur_line = cur_file, o['line']
                    else:
                        o['file'], o['line'] = cur_file, cur_line
                stack.extend(reversed(list(o.values())))
            elif type(o) is list:
                stack.extend(reversed(o))

    def _build_index(self):
        """Dump the WHOLE probe's AST once and index every FunctionDecl by name.

        82 MB of JSON and 5868 FunctionDecls for Wine's windows.h + CRT closure,
        in 1.4 s -- against 0.58 s for ONE `-ast-dump-filter` query.  See the
        BATCHING banner at the top of this file.

        In C every function declaration is a direct child of the translation
        unit, which is exactly the list `-ast-dump-filter` walks, so this sees
        the same nodes the filtered dump would have printed one at a time.
        """
        cmd = self._base() + ['-Xclang', '-ast-dump=json', self._probe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.stats['ast_dumps'] += 1
        if not r.stdout.strip():
            raise SystemExit('FATAL: clang produced no AST for the probe:\n%s'
                             % r.stderr[:4000])
        try:
            tu = json.loads(r.stdout)
        except ValueError as e:
            raise SystemExit('FATAL: cannot parse clang\'s AST dump (%s)' % e)
        self._fill_omitted_locations(tu)
        idx = {}
        for obj in tu.get('inner', ()):
            if obj.get('kind') != 'FunctionDecl':
                continue
            n = obj.get('name')
            if n:
                idx.setdefault(n, []).append(obj)
        self._index = idx
        self.stats['indexed_decls'] = sum(len(v) for v in idx.values())

    def _ast(self, name):
        if name in self._cache:
            return self._cache[name]
        if self.batch:
            if self._index is None:
                self._build_index()
            nodes = self._index.get(name, ())
        else:
            cmd = self._base() + ['-Xclang', '-ast-dump=json',
                                  '-Xclang', '-ast-dump-filter=' + name,
                                  self._probe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.stats['ast_dumps'] += 1
            nodes = self._split_json(r.stdout)
        decls = []
        for obj in nodes:
            d = self._decl_from_node(obj, name)
            if d is not None:
                decls.append(d)
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

    def _probe_chunk(self, name, ret, params, variadic=False, tag=''):
        """The lines that test ONE export.  Self-contained: it declares only
        its own `wt_r<tag>` / `wt_use<tag>`, mentions no other export, and
        needs nothing but the headers, which is what makes concatenating a
        module's worth of them into one translation unit sound.

        tag='' reproduces the single-symbol probe byte for byte, so a refusal
        message is the one this tool has always printed.
        """
        L = []
        # The ellipsis is part of the TYPE, so it has to be reconstructed too:
        # assigning &printf to `int (*)(const char *)` is a hard error, which
        # is exactly the property that makes this check worth anything.
        plist = list(params) + (['...'] if variadic else [])
        arglist = ', '.join(plist) if plist else 'void'
        # If this reconstruction is not the real prototype, this is a hard error.
        L.append('static %s (*wt_reconstructed%s)(%s) = &%s;\n'
                 % (ret, tag, arglist, name))
        L.append('void wt_use%s(void) { (void)wt_reconstructed%s; }\n' % (tag, tag))

        def intcheck(what, spelling):
            classes = INTEGER_CLASSES + (REAL_CLASS,)
            if _bare_type(spelling) in AGGREGATE_SLOT_TYPES:
                # A reviewed one-GPR aggregate (see AGGREGATE_SLOT_TYPES).  It
                # must still BE a struct or union of at most 8 bytes: if the
                # type is ever redefined out from under the review, this fails
                # the build instead of passing in the wrong register file.
                classes = (RECORD_CLASS, UNION_CLASS)
            ors = ' || '.join('__builtin_classify_type(*(%s)0) == %d'
                              % (_ptr_to(spelling), c) for c in classes)
            L.append('_Static_assert(%s, "%s: %s is not integer/pointer class");\n'
                     % (ors, name, what))
            L.append('_Static_assert(sizeof(%s) <= 8, "%s: %s is wider than 64 bits");\n'
                     % (spelling, name, what))

        for i, p in enumerate(params):
            intcheck('argument %d (%s)' % (i, p), p)
        if ret.strip() != 'void':
            intcheck('return value (%s)' % ret, ret)
        return L

    def _probe_source(self, name, ret, params, variadic=False):
        return self.probe_src + ''.join(
            self._probe_chunk(name, ret, params, variadic))

    def _compile_probe(self, name, src):
        p = os.path.join(self.workdir, 'wt_check_%s.c' % name)
        with open(p, 'w') as f:
            f.write(src)
        r = subprocess.run(self._base() + ['-Werror', p],
                           capture_output=True, text=True)
        self.stats['probe_compiles'] += 1
        return r.returncode, r.stderr

    # A clang diagnostic header line: "<file>:<line>:<col>: error: <text>".
    _DIAG_RE = re.compile(r'^(?P<file>[^\s:][^:]*):(?P<line>\d+):\d+:\s*'
                          r'(?P<kind>error|fatal error|warning|note):', re.M)

    def _probe_verdict(self, name, ret, params, variadic):
        """-> (returncode, stderr) for one export's stage-2 probe.

        Answers from the batch memo when the batch already proved this exact
        (name, ret, params, variadic) compiles clean; otherwise runs the
        single-symbol probe that has always produced the refusal message.
        """
        key = (name, ret, tuple(params), bool(variadic))
        hit = self._probe_memo.get(key)
        if hit is not None:
            return hit
        self.stats['fallback_probes'] += 1
        rc, err = self._compile_probe(
            name, self._probe_source(name, ret, params, variadic))
        self._probe_memo[key] = (rc, err)
        return rc, err

    def prefetch(self, requests):
        """Decide the stage-2 probe for a whole module in ONE compile.

        `requests` is an iterable of (export_name, allow_variadic).  Anything
        stage 1 already refuses is skipped silently -- signature() will raise
        the identical Refused when it is asked for real.

        ACCEPTS IN BULK, REFUSES ONE AT A TIME.  A clean batch memoises every
        chunk in it as accepted.  A dirty one memoises only the chunks no
        diagnostic points at, and leaves the implicated ones unmemoised so
        _probe_verdict() falls back to the single-symbol probe and produces the
        same message it always did.  If a diagnostic cannot be attributed to a
        chunk at all -- an error in a header, or a line outside every chunk --
        NOTHING is memoised and the whole batch falls back.  The batch can
        therefore only ever save work, never decide a refusal.
        """
        if not self.batch:
            return
        items, seen = [], set()
        for name, allow_variadic in requests:
            try:
                d, ret, params = self._shape(name, allow_variadic)
            except Refused:
                continue
            key = (name, ret, tuple(params), bool(d['variadic']))
            if key in self._probe_memo or key in seen:
                continue
            seen.add(key)
            items.append((key, name, ret, params, d['variadic']))
        if not items:
            return

        lines = [self.probe_src]
        nl = self.probe_src.count('\n')     # lines consumed so far
        spans = []                          # (first_line, last_line, key)
        for i, (key, name, ret, params, variadic) in enumerate(items):
            chunk = self._probe_chunk(name, ret, params, variadic, tag='_%d' % i)
            first = nl + 1
            lines += chunk
            nl += sum(c.count('\n') for c in chunk)
            spans.append((first, nl, key))

        path = os.path.join(self.workdir, 'wt_batch.c')
        with open(path, 'w') as f:
            f.write(''.join(lines))
        r = subprocess.run(
            self._base() + ['-Werror', '-ferror-limit=0', path],
            capture_output=True, text=True)
        self.stats['probe_compiles'] += 1
        self.stats['batched_probes'] += len(items)

        if r.returncode == 0:
            for _f, _l, key in spans:
                self._probe_memo[key] = (0, '')
            return

        # Attribute every error to the chunk whose lines it points at.
        bad = set()
        for m in self._DIAG_RE.finditer(r.stderr):
            if m.group('kind') == 'note':
                continue                     # notes hang off an error we saw
            if os.path.abspath(m.group('file')) != os.path.abspath(path):
                if m.group('kind') != 'warning':
                    return                   # error outside our file: fall back
                continue
            ln = int(m.group('line'))
            for first, last, key in spans:
                if first <= ln <= last:
                    bad.add(key)
                    break
            else:
                return                       # unattributable: fall back
        for _f, _l, key in spans:
            if key not in bad:
                self._probe_memo[key] = (0, '')

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

    def _shape(self, name, allow_variadic=False):
        """Stage 1 and every purely structural check, with no compiler run.

        -> (decl, ret_spelling, param_spellings), or raises Refused.  Split out
        of signature() so prefetch() can work out what the stage-2 probe would
        say WITHOUT deciding anything: the order and the wording of the checks
        below are unchanged, so a name refused here is refused identically
        whether it arrives through signature() or prefetch().
        """
        decls = self._ast(name)
        if not decls:
            raise Refused('no declaration found in Wine headers')

        shapes = {(d['qualtype'],) for d in decls}
        if len(shapes) != 1:
            raise Refused('conflicting declarations: %s'
                          % '; '.join(sorted(s[0] for s in shapes)))
        d = decls[0]
        if d['variadic'] and not allow_variadic:
            raise Refused('varargs (%s)' % d['qualtype'])
        if any(p is None for p in d['params']):
            raise Refused('unreadable parameter type in %s' % d['qualtype'])

        ret = _respell(self._return_spelling(d.get('rettype') or d['qualtype']))
        params = [_respell(p) for p in d['params']]
        if len(params) > 16:
            raise Refused('%d arguments, descriptor field holds at most 16'
                          % len(params))
        if d['variadic'] and not params:
            raise Refused('variadic with no fixed argument: nothing for the '
                          'host to anchor the va_list walk on (%s)'
                          % d['qualtype'])
        if d['variadic'] and len(params) + 1 > 16:
            # The host calls the v-variant, which takes one argument MORE than
            # the fixed count -- the va_list -- and its marshaller tops out at
            # 16 (THUNK_MAX_ARGS in dlls/ntdll/signal_ppc64.c).  Emitting this
            # would produce a descriptor the dispatcher then rejects at runtime.
            raise Refused('%d fixed arguments + va_list exceeds the 16 the '
                          'host marshaller passes' % len(params))
        return d, ret, params

    def widths(self, requests):
        """-> {key: [sizeof(param), ...]} for each (key, params) request.

        Measured by the SAME clang, target and headers that verified the
        signatures: one translation unit of `char wt_asz_<n>_<i>[sizeof(T)];`
        globals, compiled to LLVM IR, whose `[N x i8]` array lengths ARE the
        sizes on the guest target.  No parsing of type spellings, no table of
        typedef widths to maintain, and a request whose types will not compile
        simply drops out.  Returns {} on any failure: the caller treats a
        missing answer as "publish no width information", never as a guess.

        This exists because argument WIDTH decides how the host reads a guest
        argument slot -- an MS-x64 caller stores a 32-bit argument with a
        32-bit store and leaves stack garbage in the slot's upper half, which
        this port's LP64-built native code will read -- and because the one
        other place widths could come from, the .spec argument classes, is
        documented in WineSpecs below as unable to state width (`long` there
        names plenty of HANDLEs).
        """
        items = [(k, list(ps)) for k, ps in requests if ps]
        if not items:
            return {}
        lines = [self.probe_src]
        for n, (k, params) in enumerate(items):
            for i, ptype in enumerate(params):
                lines.append('char wt_asz_%d_%d[sizeof(%s)];\n' % (n, i, ptype))
        path = os.path.join(self.workdir, 'wt_widths.c')
        with open(path, 'w') as f:
            f.write(''.join(lines))
        out = os.path.join(self.workdir, 'wt_widths.ll')
        cmd = [c for c in self._base() if c != '-fsyntax-only']
        r = subprocess.run(cmd + ['-S', '-emit-llvm', '-o', out, path],
                           capture_output=True, text=True)
        self.stats['probe_compiles'] += 1
        if r.returncode != 0:
            return {}
        found = {}
        with open(out) as f:
            for m in re.finditer(r'@wt_asz_(\d+)_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 f.read()):
                found[(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
        result = {}
        for n, (k, params) in enumerate(items):
            sizes = [found.get((n, i)) for i in range(len(params))]
            if None in sizes:
                continue
            result[k] = sizes
        return result

    def signs(self, requests):
        """-> {key: [0 or 1, ...]}: is each requested type SIGNED?

        Only ever asked about types widths() already measured at 1 or 2 bytes,
        because the probe is an integer comparison and a pointer or a struct
        would not compile -- and a failed compile here costs the caller its
        answer for the whole batch.

        Measured, not spelled: `(T)-1 < (T)0` is true exactly when T is a
        signed integer type, and the two operands promote to int so the
        comparison means what it says for every sub-word width.  A bool is
        answered `unsigned`, which is right: `(bool)-1` is 1.

        This exists for the same reason widths() does, one step further on.
        Width alone says how many bits of the guest's register are real; it
        does not say what the missing bits should become.  ELFv2 requires the
        CALLER to extend a sub-word argument as its type demands -- sign for a
        SHORT, zero for a WORD -- and a ppc64 callee compiled at -O2 is
        entitled to skip re-extending.  Zero-extending everything would hand
        `SHORT nCmdShow = -1` to native code as 65535: a wrong number, not a
        crash.  libs/winecom carries the same bit for COM vtable slots
        (winecom_slot::narrowsign); this is the flat-surface half.
        """
        items = [(k, list(ts)) for k, ts in requests if ts]
        if not items:
            return {}
        return self._sign_batch(items)

    def _sign_batch(self, items):
        """One compile for `items`; on failure, bisect -- as _fp_batch does,
        and for the same reason: one unprobeable type must not cost every
        other export in the batch its answer."""
        got = self._sign_compile(items)
        if got is not None:
            return got
        if len(items) == 1:
            return {}          # caller publishes no signedness for this export
        mid = len(items) // 2
        out = self._sign_batch(items[:mid])
        out.update(self._sign_batch(items[mid:]))
        return out

    def _sign_compile(self, items):
        """-> {key: [0|1, ...]} or None if the probe did not compile."""
        lines = [self.probe_src]
        for n, (k, types) in enumerate(items):
            for i, t in enumerate(types):
                lines.append('char wt_sg_%d_%d[1 + ((%s)-1 < (%s)0)];\n'
                             % (n, i, t, t))
        path = os.path.join(self.workdir, 'wt_signs.c')
        with open(path, 'w') as f:
            f.write(''.join(lines))
        out = os.path.join(self.workdir, 'wt_signs.ll')
        cmd = [c for c in self._base() if c != '-fsyntax-only']
        r = subprocess.run(cmd + ['-S', '-emit-llvm', '-o', out, path],
                           capture_output=True, text=True)
        self.stats['probe_compiles'] += 1
        if r.returncode != 0:
            return None
        found = {}
        with open(out) as f:
            for m in re.finditer(r'@wt_sg_(\d+)_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 f.read()):
                found[(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
        result = {}
        for n, (k, types) in enumerate(items):
            lens = [found.get((n, i)) for i in range(len(types))]
            if None in lens or any(l not in (1, 2) for l in lens):
                continue
            result[k] = [l - 1 for l in lens]
        return result

    def fp_classes(self, requests):
        """-> {key: [code, ...]} where code is 0 not-FP, 1 double, 2 float.

        The same trick as widths(): one translation unit of char arrays whose
        LENGTH is the answer, read back out of the LLVM IR.  Measured by the
        same clang, target and headers that verified the signature, so a
        typedef chain (FLOAT, DOUBLE, a HRESULT that is secretly a long) cannot
        be misread the way a scan of type spellings would misread it.

        The length encodes the class rather than the class being emitted
        directly because an array bound is the only compile-time integer this
        probe can get back out of the IR:

            1 -> not floating point
            2 -> floating point, 8 bytes  (double)
            4 -> floating point, 4 bytes  (float)

        Returns {} on any failure, and the caller must then publish no
        floating-point information at all -- which refuses the export rather
        than describing it wrongly.
        """
        items = [(k, list(ts)) for k, ts in requests if ts]
        if not items:
            return {}
        return self._fp_batch(items)

    def _fp_batch(self, items):
        """One compile for `items`; on failure, bisect and retry each half.

        A single unrepresentable type in a 1275-export module would otherwise
        take the whole translation unit down, and the caller cannot treat "no
        answer" as "no floating point" -- that is exactly how a double ends up
        in a GPR.  So a failure has to be isolated to the export that caused
        it, and bisection does that in O(log n) compiles when the bad types are
        few, which they are.
        """
        got = self._fp_compile(items)
        if got is not None:
            return got
        if len(items) == 1:
            return {}          # this one export is the problem; caller refuses it
        mid = len(items) // 2
        out = self._fp_batch(items[:mid])
        out.update(self._fp_batch(items[mid:]))
        return out

    def _fp_compile(self, items):
        """-> {key: [code, ...]} or None if the probe did not compile."""
        lines = [self.probe_src]
        for n, (k, types) in enumerate(items):
            for i, t in enumerate(types):
                lines.append(
                    'char wt_fp_%d_%d[1 + (__builtin_classify_type(*(%s)0) == 8)'
                    ' + 2 * ((__builtin_classify_type(*(%s)0) == 8) && sizeof(%s) == 4)];\n'
                    % (n, i, _ptr_to(t), _ptr_to(t), t))
        path = os.path.join(self.workdir, 'wt_fp.c')
        with open(path, 'w') as f:
            f.write(''.join(lines))
        out = os.path.join(self.workdir, 'wt_fp.ll')
        cmd = [c for c in self._base() if c != '-fsyntax-only']
        r = subprocess.run(cmd + ['-S', '-emit-llvm', '-o', out, path],
                           capture_output=True, text=True)
        self.stats['probe_compiles'] += 1
        if r.returncode != 0:
            return None
        found = {}
        with open(out) as f:
            for m in re.finditer(r'@wt_fp_(\d+)_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 f.read()):
                found[(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
        result = {}
        for n, (k, types) in enumerate(items):
            lens = [found.get((n, i)) for i in range(len(types))]
            if None in lens or any(l not in (1, 2, 4) for l in lens):
                continue
            result[k] = [{1: 0, 2: 1, 4: 2}[l] for l in lens]
        return result

    def signature(self, name, allow_variadic=False):
        """-> dict(nargs, returns_void, variadic, header, line, ret, params).

        Raises Refused with a human reason for anything not representable as
        "N integer/pointer arguments, one <=64-bit integer/pointer or void
        return".  Never guesses.

        allow_variadic=True additionally permits a true `...` declaration, and
        then `nargs` is the number of FIXED arguments -- i.e. where the
        variadic part begins.  It is OPT-IN because describing a variadic is
        only sound when the caller also has a v-variant to forward to; see the
        long note at the top of this file.  Left False (the default) a
        variadic is refused exactly as it always was.
        """
        d, ret, params = self._shape(name, allow_variadic)

        rc, err = self._probe_verdict(name, ret, params, d['variadic'])
        if rc != 0:
            raise Refused(self._first_reason(err))

        hdr = d['file'] or '<unknown>'
        return dict(nargs=len(params),
                    returns_void=(ret.strip() == 'void'),
                    variadic=d['variadic'],
                    header=os.path.basename(hdr),
                    header_path=hdr,
                    line=d['line'],
                    ret=ret,
                    params=params,
                    qualtype=d['qualtype'])


# ---------------------------------------------------------------------------
# THE SOURCE-DEFINITION TIER.  Wine's implementing C source is a signature
# authority; "no corpus consumer" is not a refusal reason.
# ---------------------------------------------------------------------------
# 3,418 of the flat lane's 3,817 refusals said "no declaration found in Wine
# headers" -- ucrtbase's _Cbuild, kernelbase's AddAccessAllowedObjectAce,
# ntdll's A_SHAInit.  Every one of those exports HAS an exact signature: the
# defining function in the module's own dlls/<mod>/*.c, the very code the
# native ppc64 module was built from, read by the same clang that reads the
# headers.  Refusing them was reporting a tooling gap as an ABI fact -- the
# one thing this file must never do (its own words, PROBE_SRC note).
#
# THE PRINCIPLE IS KEPT, NOT RELAXED.  Nothing is guessed:
#   * The definition is located by name in the module's own sources (honoring
#     Makefile.in's PARENTSRC -- msvcr120 is built FROM dlls/msvcrt -- and its
#     SOURCES list and EXTRADEFS), parsed with clang for the guest target.
#   * The stage-2 probe is the SAME non-vacuous construction as the header
#     oracle's, made non-vacuous the same way: the reconstruction is appended
#     to a COPY OF THE DEFINING FILE ITSELF and ASSIGNED the real function,
#     so a wrong reconstruction is a hard compile error against the real
#     definition, in the one translation unit where its private types exist.
#   * widths / signedness / floating-point are measured in that same TU by
#     the same array-length trick, and every acceptance rule (integer class,
#     <= 64 bits, <= 16 args, the FP position limits) applies verbatim
#     downstream.
#   * A definition that cannot be found, or whose file does not parse for the
#     guest target, or that is #ifdef'd out, stays refused WITH THAT REASON.
#
# WHAT THIS TIER MUST NEVER SERVE, because service is the bug:
#   * The C++ exception machinery.  __CxxFrameHandler4 has a perfectly
#     readable C definition in dlls/msvcrt/handler4.c and serving it flat
#     would hand the native runtime a GUEST DISPATCHER_CONTEXT: language
#     handlers on this port are entered AS GUEST CODE by the SEH dispatcher
#     (dlls/ntdll/signal_ppc64.c), and every name in this family was a hole
#     ON PURPOSE -- previously enforced only by the accident of having no
#     header declaration.  The deny set below makes the hole explicit,
#     reviewed, and carries the reason into the report.
#   * Register-contract entry points (setjmp family, _chkstk): a trap stub
#     saves the wrong machine's registers.  Mostly asm-defined (so no C
#     definition is found anyway) or FORWARD'd to dlls/guestcrt; the deny
#     set is the belt to that suspenders.
#   * Anything that takes or returns a FUNCTION POINTER.  A flat serve hands
#     native code a guest code address (or the guest a native one) -- the
#     native-pc-in-guest-image crash class.  Detected on the DESUGARED
#     parameter types, so a typedef'd callback (PVECTORED_EXCEPTION_HANDLER)
#     is caught, not just a spelled-out one.  These refuse with a reason
#     naming the fix: an interception row (thunk_overrides + the callback
#     audit), not a flat thunk.

# Exact names and prefixes whose flat service would break the port's own
# execution contracts.  Each entry is a reviewed fact with its authority.
SOURCE_TIER_DENY_EXACT = frozenset((
    # SEH/unwind surface the port serves through its own dispatcher; a flat
    # trap would bypass the identity fast-path and the guest-entry contract.
    '__C_specific_handler',
    # Raises carry guest EH state; the guest C++ EH lane owns them.
    '_CxxThrowException', '__CxxThrowException',
    # Unwind helpers that walk guest frames.
    '_local_unwind', '_global_unwind2',
    # Register contracts (also FORWARD'd to guestcrt where listed by hand).
    'setjmp', '_setjmp', 'longjmp', '_longjmp', 'setjmpex', '_setjmpex',
    '_chkstk', '__chkstk', '_alloca_probe',
))
SOURCE_TIER_DENY_PREFIX = ('__Cxx', '_except_handler', '__GSHandlerCheck')
SOURCE_TIER_DENY_SUBSTR = ('FrameHandler', 'HandlerCheck')
SOURCE_TIER_DENY_REASON = (
    'guest execution contract: C++/SEH exception machinery (or a register-'
    'contract entry) must run as guest code or through the port\'s own '
    'dispatcher (dlls/ntdll/signal_ppc64.c); a flat native serve is the bug, '
    'not the fix')


def source_tier_denied(name):
    if name in SOURCE_TIER_DENY_EXACT:
        return True
    if any(name.startswith(p) for p in SOURCE_TIER_DENY_PREFIX):
        return True
    return any(s in name for s in SOURCE_TIER_DENY_SUBSTR)


class WineSourceDefs:
    """Signature authority for exports Wine declares in NO header: the
    module's OWN implementing C source.  See the tier banner above."""

    def __init__(self, source_root, dll, include_dir, generated_dir=None,
                 clang='clang', target='x86_64-windows-gnu', workdir=None,
                 defines=(), builddir=None):
        self.root = os.path.abspath(os.path.expanduser(source_root))
        self.dll = dll
        self.stem = os.path.splitext(os.path.basename(dll))[0].lower()
        self.include_dir = os.path.abspath(include_dir)
        self.generated_dir = (os.path.abspath(generated_dir)
                              if generated_dir else None)
        self.clang = clang
        self.target = target
        self.workdir = workdir or tempfile.mkdtemp(prefix='winesrcdef-')
        os.makedirs(self.workdir, exist_ok=True)
        self.builddir = os.path.abspath(builddir) if builddir else os.getcwd()
        self.defines = list(defines) + ['-D__WINESRC__']
        self.moddirs = []      # [module dir, parentsrc dir, ...]
        self.extra_incs = []   # harvested from the build Makefile's own rule
        self.files = []        # candidate .c paths, module's own first
        self._file_text = {}   # path -> text (for the name prefilter)
        self._file_defs = {}   # path -> {name: decl} or Refused for the file
        self._sig_cache = {}   # name -> sig dict or Refused
        self._file_batched = set()   # files whose export batch already ran
        self._probe_memo = {}  # (file, name) -> dict(ok/reason/sizes/fp)
        self._sign_memo = {}   # (file, name) -> signs list
        moddir = os.path.join(self.root, 'dlls', self.stem)
        if not os.path.isdir(moddir):
            return                       # tier inactive for this module
        self.moddirs = [moddir]
        extradefs, sources = self._read_makefile(moddir)
        # The BUILD tree's generated Makefile knows the module's exact
        # -I/-D set (makedep synthesizes -D_NTSYSTEM_ and the libs/
        # include dirs from IMPORTLIB/EXTRAINCL -- Makefile.in's EXTRADEFS
        # alone is an undercount, and a missing -Ilibs/symcrypt/inc turns a
        # servable sha.c into a parse refusal).  Harvested from the first
        # object rule for this module; falls back to EXTRADEFS when the
        # Makefile has no such rule.  -I/-D only: the target stays this
        # oracle's own.
        mf_inc, mf_def = self._build_makefile_flags()
        if mf_inc or mf_def:
            self.defines += mf_def
            self.extra_incs = mf_inc
        else:
            self.defines += extradefs
            self.extra_incs = []
        parent = self._parentsrc(moddir)
        if parent:
            self.moddirs.append(parent)
            pd, ps = self._read_makefile(parent)
            # PARENTSRC modules list their sources in their OWN Makefile.in
            # (resolved against the parent dir); the parent's EXTRADEFS do
            # not apply to this module's build and are not taken.
            del pd, ps
        self.files = self._candidate_files(sources)

    # ------------------------------------------------------------ discovery

    def _build_makefile_flags(self):
        """(-I dirs, -D flags) from the build Makefile's first compile rule
        for this module's objects.  Only -I and -D are taken; paths are
        resolved against the build dir the rule is written for."""
        mk = os.path.join(self.builddir, 'Makefile')
        if not os.path.isfile(mk):
            return [], []
        want = re.compile(r'^dlls/%s/[A-Za-z0-9_.-]+\.o:'
                          % re.escape(self.stem))
        try:
            with open(mk, errors='replace') as f:
                text = f.read().replace('\\\n', ' ')
        except OSError:
            return [], []
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if not want.match(line):
                continue
            for j in range(i + 1, min(i + 3, len(lines))):
                cmd = lines[j]
                if '-c ' not in cmd:
                    continue
                incs, defs = [], []
                for tok in cmd.split():
                    if tok.startswith('-I'):
                        d = tok[2:]
                        incs.append(d if os.path.isabs(d)
                                    else os.path.join(self.builddir, d))
                    elif tok.startswith('-D'):
                        defs.append(tok)
                if incs or defs:
                    return incs, defs
        return [], []

    @staticmethod
    def _parentsrc(moddir):
        mk = os.path.join(moddir, 'Makefile.in')
        if not os.path.isfile(mk):
            return None
        with open(mk, errors='replace') as f:
            for line in f:
                m = re.match(r'\s*PARENTSRC\s*=\s*(\S+)', line)
                if m:
                    return os.path.normpath(os.path.join(moddir, m.group(1)))
        return None

    @staticmethod
    def _read_makefile(moddir):
        """-> (EXTRADEFS -D flags, SOURCES basenames) from Makefile.in."""
        mk = os.path.join(moddir, 'Makefile.in')
        extradefs, sources = [], []
        if not os.path.isfile(mk):
            return extradefs, sources
        with open(mk, errors='replace') as f:
            text = f.read().replace('\\\n', ' ')
        m = re.search(r'^\s*EXTRADEFS\s*=\s*(.*)$', text, re.M)
        if m:
            extradefs = [t for t in m.group(1).split() if t.startswith('-D')]
        m = re.search(r'^\s*SOURCES\s*=\s*(.*)$', text, re.M)
        if m:
            sources = m.group(1).split()
        return extradefs, sources

    def _candidate_files(self, sources):
        """The module's PE-side .c files, module dir shadowing PARENTSRC.

        The unix half of a module is not a candidate: it compiles for the
        HOST and its declarations describe the unix boundary, not the PE
        export.  Modern Wine keeps it under <mod>/unix/ or in files that
        include config.h / wine/unixlib-shaped private headers; both marks
        are cheap to test and a false skip only costs a refusal that names
        the missing definition, never a wrong answer.
        """
        names = [s for s in sources if s.endswith('.c')]
        if not names:
            for d in self.moddirs:
                try:
                    names += sorted(os.listdir(d))
                except OSError:
                    pass
            names = [n for n in names if n.endswith('.c')]
        out, seen = [], set()
        for n in names:
            if n in seen or n.startswith('unix/'):
                continue
            seen.add(n)
            for d in self.moddirs:
                p = os.path.join(d, n)
                if os.path.isfile(p):
                    try:
                        with open(p, errors='replace') as f:
                            text = f.read()
                    except OSError:
                        break
                    if re.search(r'#\s*include\s+"config\.h"', text[:2000]):
                        break            # unix-side compilation unit
                    self._file_text[p] = text
                    out.append(p)
                    break
        return out

    # ------------------------------------------------------------ clang

    def _base(self):
        a = [self.clang, '-target', self.target, '-fsyntax-only',
             '-nostdlibinc'] + self.defines
        # The build Makefile's own -I set first (it already leads with the
        # module dir and include/); the constructed set follows as a
        # fallback for a tree whose Makefile had no rule to harvest.
        # A duplicate -I costs nothing.
        a += ['-I' + d for d in self.extra_incs]
        a += ['-I' + d for d in self.moddirs]
        a += ['-I' + os.path.join(self.builddir, 'dlls', self.stem)]
        if self.generated_dir:
            a += ['-I' + self.generated_dir]
        a += ['-I' + self.include_dir,
              '-I' + os.path.join(self.include_dir, 'msvcrt')]
        return a

    @staticmethod
    def _def_from_node(obj, name):
        """The tier's own node reader: DEFINITIONS only (a body present),
        external linkage only, and BOTH the sugared and desugared parameter
        spellings -- the desugared one is what the function-pointer guard
        reads, so a typedef'd callback type cannot slip past it."""
        if obj.get('kind') != 'FunctionDecl' or obj.get('name') != name:
            return None
        if obj.get('isImplicit') or obj.get('storageClass') == 'static':
            return None
        inner = obj.get('inner', ())
        if not any(c.get('kind') == 'CompoundStmt' for c in inner):
            return None                  # a declaration, not the definition
        ty = obj.get('type', {})
        qt = ty.get('qualType', '')
        rt = ty.get('desugaredQualType') or qt
        params, dparams = [], []
        for c in inner:
            if c.get('kind') != 'ParmVarDecl':
                continue
            pt = c.get('type', {})
            params.append(pt.get('qualType'))
            dparams.append(pt.get('desugaredQualType') or pt.get('qualType'))
        loc = obj.get('loc', {})
        file_, line = loc.get('file'), loc.get('line')
        if file_ is None:
            beg = obj.get('range', {}).get('begin', {})
            exp = beg.get('expansionLoc', beg)
            file_, line = exp.get('file'), exp.get('line')
        return dict(file=file_, line=line, qualtype=qt, rettype=rt,
                    params=params, dparams=dparams, variadic=_is_variadic(rt))

    def _defs_of(self, path):
        """{name: decl} for every external definition in `path`, cached.
        A file that does not parse caches a Refused naming the first error."""
        hit = self._file_defs.get(path)
        if hit is not None:
            return hit
        cmd = self._base() + ['-Xclang', '-ast-dump=json', path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if not r.stdout.strip():
            first = next((l for l in r.stderr.splitlines() if 'error:' in l),
                         r.stderr.strip()[:200])
            self._file_defs[path] = Refused(
                'definition file %s does not parse for the guest target (%s)'
                % (self._rel(path), first.strip()[:160]))
            return self._file_defs[path]
        try:
            tu = json.loads(r.stdout)
        except ValueError as e:
            self._file_defs[path] = Refused(
                'cannot parse clang AST for %s (%s)' % (self._rel(path), e))
            return self._file_defs[path]
        WineHeaders._fill_omitted_locations(tu)
        defs = {}
        for obj in tu.get('inner', ()):
            if obj.get('kind') != 'FunctionDecl':
                continue
            n = obj.get('name')
            if not n:
                continue
            d = self._def_from_node(obj, n)
            if d is not None:
                defs[n] = d
        self._file_defs[path] = defs
        return defs

    def _rel(self, path):
        rp = os.path.relpath(path, self.root)
        return rp if not rp.startswith('..') else path

    def find_definition(self, name):
        """-> (path, decl) or raises Refused.  Grep-prefiltered."""
        word = re.compile(r'\b%s\b' % re.escape(name))
        hits = []
        parse_fails = []
        for p in self.files:
            if not word.search(self._file_text.get(p, '')):
                continue
            defs = self._defs_of(p)
            if isinstance(defs, Refused):
                parse_fails.append(defs.reason)
                continue
            d = defs.get(name)
            if d is not None:
                hits.append((p, d))
        if len(hits) > 1:
            raise Refused('multiple external definitions of %s: %s'
                          % (name, ', '.join(self._rel(p) for p, _ in hits)))
        if not hits:
            if parse_fails:
                raise Refused('no readable definition: ' + parse_fails[0])
            raise Refused('no definition found in the module\'s own C sources '
                          '(dlls/%s%s) -- assembly, generated, or #ifdef\'d '
                          'out for this target'
                          % (self.stem,
                             ' + PARENTSRC' if len(self.moddirs) > 1 else ''))
        return hits[0]

    # ------------------------------------------------------------ the probe

    def _shape(self, name):
        if source_tier_denied(name):
            raise Refused(SOURCE_TIER_DENY_REASON)
        path, d = self.find_definition(name)
        if d['variadic']:
            raise Refused('variadic definition; the source tier serves no '
                          'variadic (cite a header and variadic=<v-variant>)')
        if any(p is None for p in d['params']):
            raise Refused('unreadable parameter type in %s' % d['qualtype'])
        for sp in list(d['dparams']) + [d['rettype']]:
            if sp and '(*' in sp:
                raise Refused(
                    'takes or returns a function pointer (%s): a flat serve '
                    'hands one machine the other\'s code address -- the '
                    'native-pc-in-guest-image class.  Needs an interception '
                    'row (thunk_overrides + the callback audit), not a thunk'
                    % sp.strip()[:80])
        ret = _respell(WineHeaders._return_spelling(d.get('rettype')
                                                    or d['qualtype']))
        params = [_respell(p) for p in d['params']]
        if len(params) > 16:
            raise Refused('%d arguments, descriptor field holds at most 16'
                          % len(params))
        return path, d, ret, params

    def _tu_with(self, path, chunks):
        """A COPY of the defining file with probe chunks appended -- the
        non-vacuous construction: &name resolves against the real definition
        in its own translation unit."""
        return self._file_text[path] + '\n\n/* spec2thunk source-tier probe */\n' \
            + ''.join(chunks)

    def _compile(self, src, out_ll=None):
        p = os.path.join(self.workdir, 'wt_srcdef.c')
        with open(p, 'w') as f:
            f.write(src)
        # NOT the header probe's blanket -Werror: this TU contains the whole
        # module source file, whose pre-existing warning-level style (a '/*'
        # inside a block comment in combase's syscom.c was the live case)
        # must not masquerade as an ABI refusal.  Warnings are silenced
        # wholesale and exactly the diagnostics the probe's soundness rests
        # on are promoted back to errors: an incompatible reconstruction, an
        # undeclared name, a bad conversion.  The _Static_asserts are errors
        # regardless.
        cmd = self._base() + [
            '-w',
            '-Werror=incompatible-pointer-types',
            '-Werror=incompatible-function-pointer-types',
            '-Werror=implicit-function-declaration',
            '-Werror=int-conversion',
            '-ferror-limit=0']
        if out_ll:
            cmd = [c for c in cmd if c != '-fsyntax-only']
            cmd += ['-S', '-emit-llvm', '-o', out_ll]
        r = subprocess.run(cmd + [p], capture_output=True, text=True)
        return r.returncode, r.stderr

    def _probe_one(self, path, name, ret, params, tag):
        """Probe chunk: reconstruction + class/width asserts (byte-compatible
        with WineHeaders._probe_chunk semantics) + width and fp arrays."""
        L = []
        arglist = ', '.join(params) if params else 'void'
        L.append('static %s (*wt_srec%s)(%s) = &%s;\n'
                 % (ret, tag, arglist, name))
        L.append('void wt_suse%s(void) { (void)wt_srec%s; }\n' % (tag, tag))

        def intcheck(what, spelling):
            classes = INTEGER_CLASSES + (REAL_CLASS,)
            if _bare_type(spelling) in AGGREGATE_SLOT_TYPES:
                classes = (RECORD_CLASS, UNION_CLASS)
            ors = ' || '.join('__builtin_classify_type(*(%s)0) == %d'
                              % (_ptr_to(spelling), c) for c in classes)
            L.append('_Static_assert(%s, "%s: %s is not integer/pointer class");\n'
                     % (ors, name, what))
            L.append('_Static_assert(sizeof(%s) <= 8, "%s: %s is wider than '
                     '64 bits");\n' % (spelling, name, what))
        for i, p in enumerate(params):
            intcheck('argument %d (%s)' % (i, p), p)
        if ret.strip() != 'void':
            intcheck('return value (%s)' % ret, ret)
        for i, p in enumerate(params):
            L.append('char wt_ssz%s_%d[sizeof(%s)];\n' % (tag, i, p))
        fpt = list(params) + ([ret] if ret.strip() != 'void' else [])
        for i, t in enumerate(fpt):
            L.append('char wt_sfp%s_%d[1 + (__builtin_classify_type(*(%s)0) == 8)'
                     ' + 2 * ((__builtin_classify_type(*(%s)0) == 8) '
                     '&& sizeof(%s) == 4)];\n'
                     % (tag, i, _ptr_to(t), _ptr_to(t), t))
        return L

    def _run_batch(self, path, items):
        """items: [(name, ret, params)].  One compile; on failure, bisect --
        one bad export must not cost the file's others their answers, and a
        refusal is produced by the export's own single probe with the
        compiler's own message (the same accept-in-bulk/refuse-one-at-a-time
        contract as WineHeaders.prefetch)."""
        chunks, tags = [], []
        for i, (name, ret, params) in enumerate(items):
            tag = '_%d' % i
            chunks += self._probe_one(path, name, ret, params, tag)
            tags.append(tag)
        out_ll = os.path.join(self.workdir, 'wt_srcdef.ll')
        rc, err = self._compile(self._tu_with(path, chunks), out_ll)
        if rc == 0:
            found = {}
            with open(out_ll) as f:
                text = f.read()
            for m in re.finditer(r'@wt_ssz(_\d+)_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 text):
                found.setdefault(('sz', m.group(1)), {})[int(m.group(2))] = \
                    int(m.group(3))
            for m in re.finditer(r'@wt_sfp(_\d+)_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 text):
                found.setdefault(('fp', m.group(1)), {})[int(m.group(2))] = \
                    int(m.group(3))
            for i, (name, ret, params) in enumerate(items):
                tag = '_%d' % i
                szd = found.get(('sz', tag), {})
                sizes = [szd.get(j) for j in range(len(params))]
                fpd = found.get(('fp', tag), {})
                nfp = len(params) + (1 if ret.strip() != 'void' else 0)
                fplens = [fpd.get(j) for j in range(nfp)]
                ok_sz = None not in sizes
                ok_fp = (None not in fplens
                         and all(l in (1, 2, 4) for l in fplens))
                if ok_sz and ok_fp:
                    self._probe_memo[(path, name)] = dict(
                        ok=True, sizes=sizes,
                        fp=[{1: 0, 2: 1, 4: 2}[l] for l in fplens])
                else:
                    # No widths => the host would read a 32-bit slot as 64
                    # (the mspatcha class); no fp answer => a double could
                    # land in a GPR.  Both are the wrong-number class, so a
                    # measurement this TU could not produce is a refusal,
                    # never a default.
                    self._probe_memo[(path, name)] = dict(
                        ok=False,
                        reason='width/fp measurement failed in the source '
                               'TU; refusing rather than publishing an '
                               'unmasked descriptor')
            return
        if len(items) == 1:
            name, ret, params = items[0]
            self._probe_memo[(path, name)] = dict(
                ok=False, reason=WineHeaders._first_reason(err))
            return
        mid = len(items) // 2
        self._run_batch(path, items[:mid])
        self._run_batch(path, items[mid:])

    def _signs_for(self, path, name, params, sizes):
        sub = [(j, params[j]) for j, sz in enumerate(sizes or [])
               if sz in (1, 2)]
        if not sub:
            return 0
        hit = self._sign_memo.get((path, name))
        if hit is not None:
            return hit
        chunks = ['char wt_ssg_%d[1 + ((%s)-1 < (%s)0)];\n' % (j, t, t)
                  for j, t in sub]
        out_ll = os.path.join(self.workdir, 'wt_srcsign.ll')
        rc, _err = self._compile(self._tu_with(path, chunks), out_ll)
        signs = 0
        if rc == 0:
            with open(out_ll) as f:
                text = f.read()
            for m in re.finditer(r'@wt_ssg_(\d+)\s*=[^\[]*\[(\d+) x i8\]',
                                 text):
                if int(m.group(2)) == 2:
                    signs |= 1 << int(m.group(1))
        self._sign_memo[(path, name)] = signs
        return signs

    # ------------------------------------------------------------------ API

    def prefetch(self, names):
        """Group the module's source-tier candidates by defining file and
        probe each file's batch once.  Purely a batching accelerator, like
        WineHeaders.prefetch: it can only save compiles, never decide."""
        by_file = {}
        for name in names:
            try:
                path, _d, ret, params = self._shape(name)
            except Refused:
                continue
            by_file.setdefault(path, []).append((name, ret, params))
        for path, items in by_file.items():
            todo = [(n, r, p) for (n, r, p) in items
                    if (path, n) not in self._probe_memo]
            if todo:
                self._run_batch(path, todo)

    def signature(self, name):
        """-> the same sig dict WineHeaders.signature returns, plus
        provenance='source-definition' and the measured sidecars
        (src_sizes / src_fp / src_signs) resolve_signatures overlays into
        its width/sign/fp stages so every downstream rule applies verbatim.
        Raises Refused with the real reason otherwise."""
        hit = self._sig_cache.get(name)
        if hit is not None:
            if isinstance(hit, Refused):
                raise hit
            return hit
        try:
            sig = self._signature_uncached(name)
        except Refused as ex:
            self._sig_cache[name] = ex
            raise
        self._sig_cache[name] = sig
        return sig

    def _signature_uncached(self, name):
        path, d, ret, params = self._shape(name)
        memo = self._probe_memo.get((path, name))
        if memo is None:
            self._run_batch(path, [(name, ret, params)])
            memo = self._probe_memo[(path, name)]
        if not memo['ok']:
            raise Refused(memo['reason'])
        rel = self._rel(path)
        return dict(nargs=len(params),
                    returns_void=(ret.strip() == 'void'),
                    variadic=False,
                    header=os.path.basename(rel),
                    header_path=rel,
                    line=d['line'],
                    ret=ret,
                    params=params,
                    qualtype=d['qualtype'],
                    provenance='source-definition',
                    src_sizes=memo['sizes'],
                    src_fp=memo['fp'],
                    src_signs=self._signs_for(path, name, params,
                                              memo['sizes']))


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
    # The subset that is still refused on the .spec's word alone, before any
    # header is consulted.  double/float are deliberately NOT here: they are
    # representable now, and only the header oracle can decide the details.
    PREFILTER_BAD_ARGS = {'int128': BAD_ARGS['int128']}

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

    def signature(self, name, specname, lineno, asserted_returns_void,
                  arch_verified=False):
        path = self._find(specname)
        with open(path) as f:
            lines = f.readlines()
        if not (1 <= lineno <= len(lines)):
            raise Refused('%s has no line %d' % (specname, lineno))
        raw = lines[lineno - 1].split('#', 1)[0].strip()
        # An export line opens with `@` or with an explicit ordinal --
        # `12  stdcall  inet_ntoa(ptr)` is how ws2_32.spec pins its classic
        # winsock ordinals, and parse_wine_spec accepts both spellings.
        first = raw.split(None, 1)[0] if raw else ''
        if first != '@' and not first.isdigit():
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
        if (' -arch=' in head or head.endswith('-arch')) and not arch_verified:
            # An -arch= line is one of SEVERAL declarations of the same name --
            # ?_type_info_dtor_internal_method@type_info@@Q*XZ has an arm, an
            # i386 and a win64 form on three consecutive lines -- so a cited
            # line number alone does not say the cited line is the one this
            # thunk's machine actually exports.  The caller can prove that,
            # because it has already run winebuild's own -arch= filter over the
            # whole .spec (spec2thunk's _spec_cpus) and knows which line
            # survived for this name; when it has checked that the cited line
            # IS that line, it passes arch_verified and this refusal does not
            # apply.  Nothing is assumed here -- the default is still to refuse.
            raise Refused('%s:%d is architecture-conditional (-arch=), so the '
                          'line cited may not be the one that applies'
                          % (specname, lineno))
        # `@ varargs printf(str)` lists only the FIXED arguments, exactly like
        # `@ cdecl` lists all of them, so counting tokens silently produced a
        # fixed-arity descriptor for a variadic.  A variadic needs its
        # v-variant cross-checked with the header oracle (nargs(impl) ==
        # nfixed+1) and a .spec has no return type to check it against, so this
        # path cannot serve one at all.  Cite the header instead.
        if 'varargs' in head.split():
            raise Refused('%s:%d is `@ varargs`; a variadic must be cited to a '
                          'header and carry variadic=<v-variant> in the thunk '
                          'spec, the .spec arity fallback cannot describe one'
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
                    variadic=False,          # refused above; never reached
                    return_verified=False,
                    header=specname,
                    header_path=path,
                    line=lineno,
                    ret='<unverified: .spec carries no return type>',
                    params=args,
                    qualtype='%s(%s)  [from %s:%d]'
                             % (name, ' '.join(args), specname, lineno))


DESC_VOID = 0x100        # bit 8
DESC_VARIADIC = 0x200    # bit 9
DESC_NARROW_SHIFT = 16   # bits 16..31: bit 16+i set means argument i is a
                         # 32-bit slot (spec class `long`/`word`); the host
                         # must take only the low 32 bits of the guest's
                         # argument slot, because an MS-x64 caller stores a
                         # 32-bit value with a 32-bit store and leaves stack
                         # garbage in the slot's upper half -- which this
                         # port's LP64-built native code WILL read.
DESC_RESERVED = 0xfffffc00   # bits 10..31, MUST be zero


def descriptor(sig):
    """Pack a signature into the uint32 that sigs_rva[i] holds.

    bits 0..7  argument count.  For a variadic this is the number of FIXED
               arguments, i.e. where the variadic part begins.
    bit  8     returns void
    bit  9     variadic -- the host must synthesise a va_list from the guest
               frame past argument nargs-1 and call impl_names_rva[i] instead.
    bits 10..15 reserved, must be zero.
    bits 16..31 narrow-argument mask: bit 16+i set means argument i is a
               32-bit slot and the host must zero-extend its low 32 bits.
               For a variadic the mask covers the FIXED arguments only.
    """
    if not (0 <= sig['nargs'] <= 16):
        raise Refused('nargs out of range')
    mask = sig.get('narrow_mask', 0)
    if mask & ~0xffff or (sig['nargs'] < 16 and mask >> sig['nargs']):
        raise Refused('narrow mask names arguments beyond nargs')
    return ((sig['nargs'] & 0xff)
            | (DESC_VOID if sig['returns_void'] else 0)
            | (DESC_VARIADIC if sig.get('variadic') else 0)
            | (mask << DESC_NARROW_SHIFT))


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
    # `--variadic` turns on the opt-in for the names that follow, the same way
    # a `variadic=<impl>` column does in a .thunks file.  Without it a variadic
    # is refused, which is the default everywhere.
    defines, allow_variadic = [], False
    while argv and argv[0].startswith('--'):
        a = argv.pop(0)
        if a.startswith('--dll='):
            defines = crt_defines_for_dll(a.split('=', 1)[1])
        elif a == '--variadic':
            allow_variadic = True
        else:
            sys.exit('usage: wine_sig.py [--dll=<name>.dll] [--variadic] '
                     '<export> ...')
    if defines:
        print('# CRT mode: %s' % ' '.join(defines))
    # find_generated_include() -- the widl-generated headers are BUILD output.
    # This used to name a DEFAULT_GENERATED that does not exist anywhere in
    # this file, so running the module directly died with a NameError before
    # it printed anything.
    generated = find_generated_include()
    if not generated:
        sys.exit('cannot find the widl-generated headers (wtypes.h); run from '
                 'the top of the build directory')
    w = WineHeaders(DEFAULT_INCLUDE, generated,
                    workdir=tempfile.mkdtemp(prefix='winesig-cli-'),
                    defines=defines)
    w.sanity()
    for n in argv:
        try:
            s = w.signature(n, allow_variadic=allow_variadic)
            print('%-26s nargs=%d void=%d variadic=%d desc=0x%03x  %s:%s  %s'
                  % (n, s['nargs'], s['returns_void'], s['variadic'],
                     descriptor(s), s['header'], s['line'], s['qualtype']))
        except Refused as e:
            print('%-26s REFUSED: %s' % (n, e.reason))
