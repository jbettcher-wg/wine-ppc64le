# What to do next

Written 2026-08-18, at the end of the session that got DOOM (2016) into
gameplay.  It is ordered: the things at the top are the ones that unblock the
most, and each entry says what is known, what is not, and where the evidence
is.  `ppc64le/games/STATUS.md` is the per-title board; this is the work list.

## Where the port stands

* **DOOM (2016) plays.** Fibers and two callback classes were the last walls;
  see the commits from `ntdll: say when the native cpu is the one executing
  guest code` onwards.
* **33 gates**, each with a negative control.  The full suite was green on the
  build DOOM runs on.  The `--sabotage` half of that sweep was interrupted and
  has not been re-run since the 226 callback rows landed — **do that first**,
  it is twenty minutes and it is the only thing between here and "the suite is
  green" being a true sentence.
* Cyberpunk 2077 reaches its own code; Boltgun reaches its engine; Portal 2,
  the first 32-bit title actually tried, does not launch and nobody has looked
  at why.

## 1. Finish the sabotage sweep

```sh
for g in ppc64le/*/check-*.sh; do "$g" --sabotage; done
```

Every gate's negative control must go red.  Two were added today
(`check-fibers.sh`, `check-callback-rows.sh`) and both pass their controls
individually; what has not been proven is that the other 31 still fail when
they should, with 226 new rows in `thunk_overrides[]` underneath them.

## 2. Portal 2, and the 32-bit lane in general

Portal 2 (appid 620) was launched on 2026-08-18 and did not start.  Nothing
has been diagnosed.  Logs are in `steamapps/compatdata/620/`.

It matters beyond one game: Half-Life 2 and its episodes, FreeInfantry and the
Win32 build of Styx are all PE32, and the WoW64 lane has never carried a real
game — only `check-wow64-smoke.sh`.  Whatever Portal 2 hits is likely to be
what all of them hit.

## 3. The four things Cyberpunk is waiting on

All named, none deep (`ppc64le/games/STATUS.md` has the run):

* four kernel32 exports the thunk generator refused —
  `GetPhysicallyInstalledSystemMemory`, `RaiseFailFastException`,
  `SetThreadStackGuarantee`, `SetThreadSelectedCpuSets`.  They are in
  `kernel32.spec`, so the refusal reason is in the generator's own report;
  read it before assuming they are hard.
* `ws2_32` ordinal 12.
* six C++ RTTI/EH entry points that the **real** `vcruntime140` forwards into
  `ucrtbase` and this tree's guest `ucrtbase` does not export:
  `__RTtypeid`, `__std_type_info_name`, `__std_type_info_destroy_list`,
  `__std_exception_copy`, `__std_exception_destroy`, `_CxxThrowException`.
  The last one deserves thought rather than a row: it is the C++ throw entry,
  and this port deliberately refuses `__CxxFrameHandler3` because the EH
  personality belongs to the guest.

## 4. The trampoline pool stops at six arguments

`ppc64le/thunks/callback_holes.txt` lists 24 exports with no wrapping row.
Fourteen of them are waiting on one thing: `wrap_guest_callback_ex` has
fixed-arity dispatchers for four, five and six arguments and refuses anything
else by name.  Extending it to seven, eight and nine closes
`SetWinEventHook` (7), `DdeInitialize` (8), `WSAAccept` (8), `EventRegister`
(7) and the `CopyFileEx`/`MoveFileWithProgress` family (9).  The pattern for
five and six is already in the file; this is mechanical.

## 5. Make a new title's first run boring

The tools exist and are not yet joined up:

* `ppc64le/games/library_sweep.py --audit` reads the whole Steam library and
  says what will happen to each title, in under two seconds.
* `ppc64le/thunks/import_chain.py` does the same for one binary anywhere on
  disk, which is how Cyberpunk's three missing modules were found in seconds
  after the run had already cost minutes.
* What is missing is the loop: a title that needs a per-game setting still
  needs somebody to know that.  `ppc64le/steamtool/appconfig/<appid>.env` is
  the mechanism (DOOM has one); filling it in from sweep results rather than
  from failed launches is the difference between this working for other people
  and working for its author.

## 6. Performance: the ~2.5-core ceiling

Unchanged and unexplained: latency-bound, ~54,000 wakeups/s, while an emulated
GE-Proton on the same machine uses 7-9 cores.  DOOM at 500%+ CPU during load
suggests the ceiling is not where it was measured, so **re-measure before
theorising**.  The NUMA lever (`WINE_PPC64LE_NUMA_NODE`) is in place and
unmeasured: bound and unbound runs of the same scene, frame times compared, is
an afternoon's work and would settle whether it is worth a default.

## 7. Smaller, known, and written down

* `SetThreadGroupAffinity(group 1)` needs a `group` field in
  `server/protocol.def`; `check-cpu-topology.sh` reports it as a LIMIT and
  re-arms itself when the field appears.
* Quake II is on a library drive that is not mounted.
* `mfmediaengine`, `evr`, `wmvcore` have a built COM surface no title has
  driven.
* The callback audit cannot see a callback that arrives inside a **struct**
  (a `WNDPROC` in a `WNDCLASSEX`); those rows carry handlers and are found the
  hard way.  If a future crash names one, add it to
  `check-callback-rows.sh`'s reasoning rather than only to the table.
