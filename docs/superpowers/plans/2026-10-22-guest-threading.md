# Guest threading: plan (phase 1)

Spec: `docs/superpowers/specs/2026-10-22-guest-threading-design.md`. Branches
`threading/N-name`, stacked, cut from `upstream/canary_experimental`; the top
one goes in the integration list (`xenia-canary-tooling/config/integration.txt`)
for testing. Fork-only until the owner says otherwise.

## Common to every step

- `git clang-format` 20, the fork check (`xenia-canary-tooling/check.ps1`),
  kernel tests (`xenia-kernel-tests "[dispatch]"` and the full suite).
- Smoke run on the integration build with the step, against the integration
  build without it: each title in `smoke.txt` booted headless for 60 s
  (no agent); record final state, first-swap time, swaps, crash, stall, fps.
  Titles: the acceptance set (45410869, 535107D4, 4B4D07D1, 545407DC,
  454109C9, 45410950 disc 1) and the probe regression set
  (`%LOCALAPPDATA%\game-mcp-probe\regression-playable.txt`), plus 454107D9.
- A step passes when no title gets worse (new crash, new stall, later first
  swap by more than 2x, fps down more than 10%) and its unit tests pass.

## Step 1: the core (threading/1-dispatch-core)

- `src/xenia/kernel/dispatch/dispatcher.h/.cc`: `ObjectId`, `ThreadId`,
  `ObjectKind` (notification event, synchronization event, semaphore, mutant,
  thread), `ObjectState` (signal count, limit, owner, recursion, abandoned,
  waiter queue), `ThreadState` (wait: objects, any/all, deadline, alertable,
  status; guest suspend count; emulator stops; pending APC flags),
  `Effects` (wakes with status, header writes), operations per spec §4.1.
- No users. Tests: `src/xenia/kernel/testing/dispatch_test.cc`, every rule in
  spec §3.

## Step 2: shell plumbing (threading/2-park)

- `XThread` gets a `ThreadId` in a process-wide `Dispatcher` owned by
  `KernelState`, and a park primitive (`threading::Event`, auto-reset).
- `kernel/dispatch/shell.h/.cc`: `DispatchLock`, `WaitOnCore(thread, objects,
  any/all, timeout, alertable)` (lock, BeginWait, park until woken), `Apply
  (effects)` (header writes, unparks).
- Unused by objects yet; a unit test drives two host threads through
  `WaitOnCore` + `Signal` on a fake object.

## Step 3: events (threading/3-events)

- `XEvent` holds an `ObjectId`; `Set/Pulse/Reset/Query` go through the core;
  guest header write-through; `SyncFromGuest` becomes `SetSignalState`, called
  from every export that receives the event.
- `XObject::Wait` / `WaitMultiple` / `SignalAndWait`: if every object is
  core-backed, use `WaitOnCore`; otherwise the old host path.
- Critical sections ride on this (their embedded event).
- `KeSetEvent` returns the previous state.
- Linux check on the Arc server image.

## Step 4: semaphores (threading/4-semaphores)

- `XSemaphore` through the core; `KeReleaseSemaphore`/`NtReleaseSemaphore`
  report limit exceeded from the core.

## Step 5: mutants (threading/5-mutants)

- `XMutant` through the core: owner `ThreadId`, recursion, abandonment.
- `KeInitializeMutant`, `KeReleaseMutant` exports; guest `X_KMUTANT` header
  (owner, abandoned) written through; `GetNativeObject` creates native
  mutants.

## Step 6: thread objects (threading/6-thread-objects)

- `XThread` as waitable through the core: signaled at `Exit`/`Terminate`;
  owned mutants abandoned (spec §3).
- With every kind core-backed, remove the old host-primitive wait path and
  the `WaitScope` records (the threads op reads the core).
- Linux check.

## Step 7: guest-time deadlines (threading/7-guest-time)

- Deadline timer in the shell on guest system time; absolute and relative
  timeouts; `KeDelayExecutionThread` with absolute intervals.

## Step 8: APCs (threading/8-apcs)

- `QueueApc` in the core; `KeInsertQueueApc` wakes; pending check before
  blocking; kernel APCs delivered; POSIX parity (no host signals for APCs).

## Step 9: stops and suspension (threading/9-stops)

- Emulator stop vs guest suspend in the core; `Emulator::Pause` through it;
  audio and vblank workers take the stop. Remove the per-subsystem pause
  handshakes that become redundant.
- Linux check.

## Steps 10 to 12

Timers (10), interrupts and DPCs (11), priorities and boosts (12), as in the
spec §6; each planned in detail when step 9 lands, with what steps 1 to 9
taught.

## Phase 2

Exceptions: a separate spec on top of the core's thread state.
