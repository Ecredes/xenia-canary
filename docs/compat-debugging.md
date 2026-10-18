# Debugging a game compatibility issue

How to go from a compat finding ("title X hangs on its first load") to a fix
that can go upstream. Learned on fork #46-#48 (4541094A, 494707E4, 544307D1)
in October 2026. Refer to games by title ID only.

The short version: **find the one guest call that decides the outcome, read
its caller in Ghidra, and change only what the caller needs.** Logs tell you
where things stop; the game's own code tells you why.

## 0. Ground rules

- **Clean room.** Never use XDK headers, docs or samples, even when an issue
  says "look it up in the XDK". Behavior comes from the game's code (Ghidra),
  runtime observation, public material (Free60), and other emulators' source
  (xenia-edge is BSD-3).
- **One worktree per experiment.** Other sessions run the `testing/telemetry`
  worktree's exe as "the telemetry build". Never rebuild it with experiments.
  Use `debug/guest-stack-dump` (worktree `debug-stack-dump`) for diagnostics,
  and a fresh topic branch from `upstream/canary_experimental` for the fix.
- **Scripts go in their own folder.** A `dis.py` or `bisect.py` next to a
  Python script shadows the standard library module of the same name.

## 1. Reproduce with a verified route

1. Drive the route in game-mcp with screen checks: `wait_for` "stable" or
   "change" plus a `snapshot` at each step. Check every snapshot: fixed waits
   drift when boot timing changes (debug logging, another build), and a
   missed press makes a run look like a hang.
2. Save it as a title macro (`game_macro_save`, `title_id`). Split long
   routes at stable screens (boot to notice, notice to load) so retries are
   cheap.
3. Reproduce at least twice. Record with `record=true` when the run matters.
4. Note the symptom precisely from telemetry, not the screenshot: swaps
   still advancing (loading screen animates) vs frozen, file reads stopped
   or not, blocked/sleeping thread counts.

## 2. Classify the stall

Run the diagnostic build with `--guest_stack_dump_interval=15` (seconds) and
read two or three consecutive dumps:

| Pattern | Meaning | Next |
|---|---|---|
| Same thread blocked in the same kernel wait every dump | Deadlock or missing signal | Find who should signal that object |
| Threads in guest code with changing PCs, yields between | Livelock / polling | Decompile the polling loop: what condition? |
| Expected thread missing from later dumps | It exited (check `ExTerminateThread` code) | Read its last calls |
| A thread in a host lock (`RtlAcquireSRWLockExclusive`) | Host contention | Usually a symptom, not the cause |

Lesson from 4541094A: the last log line before a stall is a weak clue. The
unimplemented XCTD query was the last line and turned out to be irrelevant.

## 3. Find the deciding call

- **Undefined externs first.** Canary's `UndefinedCallExtern` doesn't set r3,
  so the guest sees its first argument as the return value: usually a
  non-zero pointer that reads as success. 544307D1 looped forever on this.
  Telemetry lists them as `extern_undefined_call` / `import_missing`.
- **Stubs that return failure.** `export_first_call tag=stub` tells you a stub
  ran, not what it returned. Check the stub's code.
- **Debug log for arguments:** `log_level=3` shows most kernel calls with
  arguments. Do not use `log_high_frequency_kernel_calls` for long runs: the
  logger's ring buffer saturates and causes its own freeze.
- **Caller address:** log the guest `lr` in the export handler
  (`cpu::ThreadState::Get()->context()->lr`), or read it from the stack dump.
  The import thunks are `sc; blr`, so `lr` lands in the real caller.
- **Missing exports:** a logging stub (`LogRawCall` on the debug branch)
  prints r3-r10 and `lr` for an export canary doesn't have, and returns a
  configurable status.

## 4. Read the caller in Ghidra

Setup (once): Ghidra 12.1.2 at `C:\Tools\ghidra_12.1.2_PUBLIC`, XEXLoaderWV
release `12.1.2` unzipped into
`%APPDATA%\ghidra\ghidra_12.1.2_PUBLIC\Extensions`. Scripts are in
`re/ghidra` (copies in `C:\Tools\ghidra_xenia\scripts`).

1. Extract the module: `python -I re/tools/xdvdfs.py <iso> get default.xex
   out.xex` (`list` shows all files; upstream `xenia-vfs-dump` doesn't build,
   fork #50).
2. Import and analyze (about 3-7 minutes):
   `analyzeHeadless.bat <projdir> <proj> -import out.xex -max-cpu 2`
3. **Then** fix the register save/restore helpers, with analysis off:
   `-process out.xex -noanalysis -scriptPath re\ghidra -preScript
   FixSaveRestGpr.java`. Ghidra marks `__savegprlr_N` as non-returning and
   cuts off almost every function. Running analysis again re-breaks it, so
   always use `-noanalysis` afterwards.
4. Decompile: `-postScript DecompileAt.java <out.c> <addr...>` (the function
   containing each address) or `DecompileCallers.java <out.c> <addr>` (every
   function referencing an address, such as an import thunk). Import thunk
   addresses are in the Xenia log's import table (`F <record> <thunk> ...`).

What to look for: how the caller branches on the result (`< 0`, `== 2`,
`& 1`), what it falls back to, and how it passes arguments. Real examples:

- XCTD query fails, so the game sniffs the file's magic itself. Implementing
  the query changes nothing (4541094A).
- `StfsCreateDevice` fails, so the game retries once, then skips the cache.
  A clean failure is the whole fix (544307D1).
- `XNetInAddrToXnAddr`'s caller loads the IN_ADDR value and passes it, so
  canary's pointer signature was wrong (494707E4).

Quick look without Ghidra: `re/tools/ppcdis.py` (Capstone) and
`ppccallers.py` (`bl` scan) over an image dumped at runtime with
`--guest_image_dump_dir`.

## 5. Compare with xenia-edge, carefully

- Run the latest edge release on the same route. Edge spawns a new process
  on title launches and crashes into a dialog; game-mcp loses the window in
  both cases (game-mcp #14). Edge needs `cvars={"hid":"sdl"}`.
- Edge passing tells you the bug is fixable, not where the fix is. Toggle
  edge's big features (`guest_scheduler=false`) before blaming them.
- Edge bisects need several runs per step: older edge builds crash
  intermittently at the same spots canary hangs.
- Edge's code can be read with `git grep <symbol> edge/edge` (remote `edge`).

## 6. Make the fix minimal and prove each part

- Experiment on the debug branch first. When a fix has several parts, add a
  temporary bitmask cvar that turns each part off, and run the route once per
  bit. Ship only the parts whose removal brings the bug back.
- Rebuild the fix on a topic branch from upstream, one commit per concern,
  each building alone (`xb build --target=xenia-kernel` per commit).
- Provenance goes in the commit message (title ID, addresses, observed
  arguments and results); one-line comments and `TODO(Ecredes)` in code.
- Run `check.ps1` (exit 0), build on Linux in WSL, and verify the final
  commit in game with the saved route.
- Push to the fork, record the result on the compat issue and fork #28. Ask
  before any upstream PR.

## 7. When it goes wrong

- A run "passes" or "fails" once: run it again before acting on it.
- A session crash can leave NUL-filled refs or truncated loose objects, both
  in the repo and in the fork checker's clone (`~/src/xenia-fork-check` in
  WSL). Check with `git fsck`; the checker reports broken fetches as exit 2.
