# Guest threading: survey and failure catalog

Status: survey (step 1 of the threading redesign), 2026-10-22. Fork-first
(Ecredes/xenia-canary). Nothing goes upstream without the owner's say-so.
This document describes what exists and what fails; the design comes next,
after the decisions in §6.

Paths are under `src/xenia/` at the integration build (upstream e6cd2e4bc
plus every fork branch, including the memory-coherence series and the
automation stack).

## 1. Intent

Xenia's guest kernel threading (about 9,000 lines: `kernel/xthread.*`,
`xobject.*`, `xevent.*`, `xsemaphore.*`, `xmutant.*`, `xtimer.*`,
`xboxkrnl/xboxkrnl_threading.cc`, `base/threading_{win,posix}.cc`,
`kernel/kernel_state.cc`) maps each guest kernel concept onto the nearest
host primitive: a guest event is a host event, a mutant a host mutex, a
suspend a host `SuspendThread`, a timer a host waitable timer, an APC a host
APC or signal. The guest kernel's own rules (who wakes, in what order, what a
suspend count means, when an APC runs, what time a timeout is measured in)
are therefore decided by Windows or by Xenia's POSIX emulation of Windows,
differently on each, and are invisible to Xenia itself.

The redesign's goal, as with GPU↔CPU memory coherence: model the Xbox 360
kernel's dispatcher as a functional core (plain data, pure decisions,
unit-tested) and make the host threads an imperative shell that carries out
its decisions, the same on Windows and Linux.

## 2. How it works today

### 2.1 Dispatcher objects

| Object | Host primitive | Where its state lives |
|---|---|---|
| Event | `threading::Event` (Win32 event; POSIX mutex+condvar) | host event, a shadow `host_signaled_`, and the guest header; `Query` reads the host (`kernel/xevent.cc:36-141`) |
| Semaphore | `threading::Semaphore` | host count, shadow `host_count_`, guest header (`kernel/xsemaphore.cc:50-142`) |
| Mutant | `threading::Mutant` (Win32 mutex; POSIX count + `std::thread::id`) | host primitive (ownership, recursion); `owning_thread_` is advisory; no guest `X_KMUTANT` at all (`kernel/xmutant.cc:27-58`) |
| Timer | `threading::Timer` (Win32 waitable timer; POSIX timer queue) | host; no guest header (`kernel/xtimer.cc:23-97`) |
| Thread (as waitable) | the host thread handle | host exit, separate from guest `signal_state` (`kernel/xthread.h:577`, `xthread.cc:505,551`) |

- Guest-allocated objects (`KEVENT`, `KSEMAPHORE`, critical sections) are
  bound to an XObject by writing a signature and handle into the guest
  header's `wait_list` ("stash", `kernel/xobject.h:261-264`) and created
  lazily in `GetNativeObject` (`kernel/xobject.cc:452-547`). Only events and
  semaphores can be created that way; guest mutants, timers, threads, queues
  hit `assert_always` (`xobject.cc:508-534`).
- Guest writes to a header are picked up (`SyncFromGuest`) only when the guest
  passes the object to a `Ke*` export; never for `Nt*` handle calls, and never
  for a title that polls or writes `signal_state` itself.
- Not implemented: `KeSetTimer(Ex)`, `KeCancelTimer`, `KeInitializeMutant`,
  `KeReleaseMutant`, `KeAlertThread` (table only, `xboxkrnl_table.inc`);
  mutant abandonment (`assert_false(abandon)`, `xmutant.cc:51-52`);
  `DeleteMutant`/`DeleteTimer` call `FatalError` (`kernel_state.cc:1531-1539`).

### 2.2 Waits and signals

- `XObject::Wait`, `SignalAndWait`, `WaitMultiple` (`kernel/xobject.cc:217-401`)
  call `threading::Wait/SignalAndWait/WaitAny/WaitAll` on host handles. The
  auto-reset consume and semaphore decrement happen inside the host wait; the
  guest header is fixed up afterwards (`WaitCallback`), not atomically.
- `SignalAndWait` signals the host handle directly, bypassing `XEvent::Set` /
  `ReleaseSemaphore`: the guest header and shadow are never updated
  (`xobject.cc:283-285`). On POSIX the signal and the wait are separate steps.
- WaitAll: `WaitForMultipleObjectsEx` on Windows; on POSIX try-lock every
  handle and poll a global epoch in steps of up to 50 ms
  (`base/threading_posix.cc:327-447`).
- Timeouts: guest ticks to host milliseconds (`TimeoutTicksToMs`,
  `xobject.cc:198-208`): positive (absolute) timeouts are read as raw ms; under
  1 ms becomes 0; scaled by the time scalar; then measured by the host wait in
  **host** time. Absolute delays hit `assert_always` (`xthread.cc:1032-1036`).
- `KeSetEvent` returns 1 instead of the previous state; `NtReleaseMutant`
  ignores its result; `KeReleaseSemaphore` ignores release failure
  (`xboxkrnl_threading.cc:578-903`).
- Priority boost on wake: the last signaler's increment, stored on the object
  (`xobject.h:236-240`), applied by `BoostOnWake` even when the wait was
  interrupted by an APC (`xobject.cc:245-248`, `xthread.cc:793-847`).
- Critical sections (`xboxkrnl_rtl.cc:536-684`): spin, then wait on the embedded
  synchronization event (created lazily as an XEvent); which waiter wakes is
  the host auto-reset event's choice.

### 2.3 Threads, scheduling, suspension

- One host thread per guest thread, all truly parallel; there is no guest run
  queue. The 6 hardware threads are a number in KPCR/KTHREAD unless host
  affinity is applied (`ignore_thread_affinities` defaults true,
  `xthread.cc:32,849-881`).
- Priority: 5 host priority buckets (`GuestPriorityToHost`,
  `xthread.cc:721-733`); `ignore_thread_priorities` updated to true on
  2026-04-09. Decay is wall-clock (a 1 ms timer, every 20 ticks,
  `kernel_state.cc:1295-1302`). `KeSetBasePriorityThread` and
  `KeQueryBasePriorityThread` return the host priority, not the guest's.
- KTHREAD `thread_state` is written once (INITIALIZED) and never updated
  (`xthread.cc:214`).
- Guest suspend (`NtSuspendThread`, `KeSuspendThread`, `DmSuspendThread`)
  updates KTHREAD `suspend_count` and host-suspends (`xthread.cc:915-993`); on
  Win32, `Resume` can wrap the count to 255. Emulator pause, the debugger,
  `RunGuestThreadUntil`, `TerminateTitle` and save state host-suspend without
  touching the guest count, so the two counts drift apart. Win32
  `SuspendThread` is asynchronous with no safe point.
- APCs: `KeInsertQueueApc` links the APC but never wakes the target (TODO,
  `xboxkrnl_threading.cc:1580`); `NtQueueApcThread` wakes it with a host APC
  (Win32 `QueueUserAPC`) or signal (POSIX, one slot that the next call
  overwrites). Kernel-mode APCs are never delivered, only discarded at rundown
  (`:1513`). No wait checks for pending user APCs before blocking. On POSIX an
  alertable wait cannot be interrupted at all (`threading_posix.cc:294-325`,
  `:1594-1600`; `AlertableSleep` always returns success, `:214`).
- DPCs run inline on the thread that queues them, under the global lock
  (`xboxkrnl_threading.cc:1632-1682`). Graphics interrupts run the guest's
  callback on whatever thread raised them: the vblank host thread
  (`gpu/graphics_system.cc:452-463`) or the command processor worker (PM4
  `INTERRUPT`), impersonating a DPC (`kernel_state.cc:1392-1448`), and leave
  that thread's active CPU changed.
- Guest code also runs on host threads that are not guest threads: the audio
  worker runs the title's audio callbacks (`apu/audio_system.cc:204`), the
  vblank thread runs the interrupt callback. Emulator pause does not suspend
  these; each has its own pause handshake.
- One process-wide recursive lock, `global_critical_region` ("like disabling
  interrupts", `base/mutex.h:180-188`), guards object creation and lookup,
  handle lookups, the DPC list and the object table. There is no dispatcher
  lock.

### 2.4 Exceptions

- Host exception handlers convert access violations and illegal instructions
  only (`base/exception_handler_win.cc:95-121`); `Emulator::ExceptionCallback`
  treats every guest fault as fatal: stack, telemetry, dump, `Pause()`, a
  dialog, self-suspend (`emulator.cc:1411-1566`).
- There is no guest exception dispatch. `RtlRaiseException` logs C++
  exceptions (E06D7363) and returns; `RtlUnwind`, `RtlUnwind2`,
  `RtlVirtualUnwind`, `RtlLookupFunctionEntry`, `RtlCaptureContext`,
  `__C_specific_handler` are missing (`xboxkrnl_debug.cc:103-147`,
  `xboxkrnl_table.inc`). `.pdata` is read only to seed code analysis
  (`cpu/xex_module.cc:1675-1692`).
- Traps (`tw`/`twi`) log and continue, or emit `int3` (`ppc_emit_control.cc`,
  `x64_emitter.cc:442-463`).

### 2.5 Time

- Guest time: `Clock::QueryGuestTickCount` (50 MHz), scaled; stops while the
  emulator is paused (fork, automation/8-pause). `QueryGuestInterruptTime`
  returns host interrupt time and does not stop (`base/clock.cc:210-212`).
  `KeTimeStampBundle` is updated by a 1 ms host timer.
- Wait timeouts and timers run in host time (§2.2), so they keep counting
  through a pause, and a host timer's callback runs only when the arming
  thread enters an alertable host wait (Win32) or on the timer thread (POSIX).

## 3. Failure catalog

Evidence from 2026-10-15 to 2026-10-22. "Fork fix" means a branch on
Ecredes/xenia-canary.

| # | Failure | Evidence | Class |
|---|---|---|---|
| F1 | `Emulator::Resume` resumed the threads that were *not* running (created-suspended ones started early; the title's own stayed suspended) | 454107D9 crash at 0x825923A8 after a channel pause; fork fix bd1e0290c | emulator suspend and guest suspend share host suspend |
| F2 | Pause hangs: the XMA decoder's idle worker never saw the pause; stopping vblanks before the pause completed deadlocked the GPU and audio handshakes | 454107D9 headless; fork fixes 158bf97c0, fb166caf9 | ad-hoc per-subsystem pause handshakes |
| F3 | A paused guest still sees host time pass in waits and timers | design reading (§2.5); guest clock stop is fork-only | timeouts in host time |
| F4 | No guest exception dispatch: 45410869 installs an unhandled exception filter and faults on purpose at 0x821D0634 to run its main loop from it; 535107D4 throws a C++ exception (E06D7363); 4B4D07D1 crashes after raising 406D1388 (SetThreadName) | crash stacks 2026-10-22 (fork a94168dbd), probe batch 2026-10-19 | exceptions |
| F5 | A stale handle freed a live thread (GetNativeObject on a stash whose handle was recycled) | fork #46, `kernel/stale-stash-handle` | object identity lives in guest memory |
| F6 | A timed wait on a terminated thread blocked on its host exit | `threading/wait-no-blocking-join` (800767b75) | thread-as-waitable is the host handle |
| F7 | Signal state can't be observed without side effects (probing a host auto-reset event consumes it) | threads op design, fork automation/10-threads | state inside host primitives |
| F8 | Guest code on host threads that aren't guest threads (audio callbacks, vblank interrupt) escapes emulator pause and runs with borrowed identity | §2.3; pause work 2026-10-21 | interrupts and callbacks |
| F9 | A suspended worker held the lwarx reservation bit | fork #35, `cpu/reservation-generations` | suspension vs atomics |
| F10 | A stale `launch_data.bin` hijacks the next launch; title relaunch is not an in-place swap | fork #53 | title lifecycle |
| F11 | Frostbite titles (454109C9, 45410950): GPU `WAIT_REG_MEM` stuck on a memory value with the main thread running; repeats on 45410950: the automation-only build hung 5 of 6 runs, the integration build 0 of 6, so the coherence series fixes it and it is not a threading defect | threads snapshots 2026-10-22 | CPU/GPU fences (coherence handoff, §5) |
| F12 | 545407DC: hangs at menus in one run, crashes (null+0x18, thread 23) in another | probe batch 2026-10-19, batch 2026-10-22 | nondeterministic: scheduling or timing sensitive |
| F13 | 45410950 on the integration build: crash null+0x62 in BF.Main after calling the undefined `XamBackgroundDownloadSetMode` | batch 2026-10-22 | unknown yet |

## 4. Guest semantics decided by the host today

The redesign has to take each of these back:

1. Which waiter wakes, and in what order (no guest wait lists; `wait_list` is
   reused for the stash).
2. Auto-reset consume and semaphore decrement, atomically with the wake.
3. Mutant ownership, recursion and abandonment.
4. Wait-all and signal-and-wait atomicity.
5. Pulse.
6. When an APC wakes an alertable wait and when it runs; kernel APCs at all.
7. Timer expiry, and which thread runs the timer's APC.
8. A thread object's signaled state.
9. Timeout units and the clock they run on.
10. Signal state as the guest sees it, including guest writes to headers.
11. Suspension: the guest count vs every other reason a thread is stopped.
12. Running, preemption, priority and affinity, on 6 hardware threads.
13. Where DPCs and interrupts run.
14. What happens on a guest exception.

## 5. Handed off by the memory-coherence design

From `design/memory-coherence`, §7 of its spec:

- Rules for a guest thread blocked inside a fault handler: what `XThread`
  suspend, APC delivery and the kernel's reported thread state mean there.
- A written lock hierarchy around `global_critical_region` (coherence releases
  it while waiting and holds a tracker mutex as a leaf lock).
- Ordering for delivering graphics interrupts to guest threads.

## 6. Decisions for the design

1. **Scope of phase 1.** All dispatcher objects at once, or events, semaphores
   and mutants first (the ones titles use most), with timers, queues and
   thread objects after?
2. **Who runs a guest thread.** Keep one host thread per guest thread, with
   the core deciding wakes (each host thread blocks on one park primitive), or
   go further and schedule guest threads onto 6 host threads as the hardware
   does (exact priorities and preemption, but a far larger change)?
3. **Exceptions in phase 1 or a later phase.** Dispatch through `.pdata`
   handlers, `RtlRaiseException`, `RtlUnwind`: three crash titles depend on it
   (F4).
4. **Priorities and boosts:** model them in the core (deterministic, matches
   the hardware's rules) or leave priority to the host as now?
5. **Time:** run all timeouts and timers on guest time (they then stop while
   paused and follow the time scalar exactly)?
6. **Pause and suspend:** one core state for "stopped by the emulator",
   separate from the guest's suspend count, replacing the per-subsystem
   handshakes, including the audio and GPU workers that run guest code?
7. **Interrupts and DPCs:** deliver them on a guest-visible thread with the
   guest's processor and IRQL, rather than on the raising host thread?

## 7. Next

After the decisions: a design spec (`docs/superpowers/specs/`), then a commit
plan, each step A/B-tested on the integration build with headless batches and
the threads snapshot. Acceptance titles: 45410869, 535107D4, 4B4D07D1 (F4),
545407DC (F12), 454109C9 and 45410950 (F11, F13), plus the regression set from
the 2026-10-19 probe batch.
