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

## Step 2: every waitable through the core (threading/2-core-waitables)

Replanned while implementing: steps 2 to 8 as first written moved one object
kind at a time, with the old host-primitive path kept for waits that mix
core and host objects. That can't be made correct: a wait-any over a core
event and a host event has no single place that decides it, and wait-all
can't be atomic across the two. So every waitable moves at once.

- `kernel/dispatch/shell.h/.cc`: the lock, header write-through, a `Waiter`
  per host thread (core thread id plus one auto-reset park event),
  `Wait`/`SignalAndWait`, `DeadlineFromTimeout` (negative relative, zero a
  poll, positive absolute guest system time). A parked waiter with a
  deadline rechecks guest time at least once a second (pauses, scalar).
- `XEvent`, `XSemaphore`, `XMutant` (owner is the waiter's core thread),
  thread objects (signaled at exit/terminate, owned mutants abandoned),
  `XTimer` (core event armed by the platform timer queue; the routine is
  queued as a user APC on the setting thread), `XFile` I/O completion and
  `XNotifyListener` (core events). `GetWaitHandle`/`WaitCallback` removed.
- User APCs: `KeInsertQueueApc` tells the core under the APC lock, so an
  alertable wait returns `STATUS_USER_APC`; `xeProcessUserApcs` clears the
  pending flag under the same lock.
- `KeSetEvent`/`KePulseEvent`/`KeResetEvent` return the previous state;
  `WSAWaitForMultipleEvents` passes a relative timeout.
- Still on host primitives: `KeDelayExecutionThread` (host alertable sleep,
  woken by `QueueUserCallback`) and I/O completion ports.

## Step 3: delays and kernel APCs (threading/3-delay-apcs)

- `KeDelayExecutionThread` as a wait on no objects with a deadline, so it
  shares the guest clock and the user-APC rule; drop the host callback wake.
- Kernel APCs delivered at the wait boundary; POSIX parity (no host signals
  for APCs).

## Step 4: stops and suspension (threading/4-stops)

- Emulator stop vs guest suspend in the core; `Emulator::Pause` through it;
  audio and vblank workers take the stop. Remove the per-subsystem pause
  handshakes that become redundant.
- Linux check.

## Steps 5 to 7

I/O completion ports (5), interrupts and DPCs (6), priorities and boosts
(7), as in the spec §6; each planned in detail when step 4 lands. Timers
moved into step 2.

## Phase 2

Exceptions: a separate spec on top of the core's thread state.
