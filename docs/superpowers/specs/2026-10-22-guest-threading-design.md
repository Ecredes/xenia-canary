# Guest threading: design

Status: decisions approved by the owner, 2026-10-22 (survey §6, all seven as
recommended). Fork-first (Ecredes/xenia-canary), built as small commits that
could each go upstream later. Nothing goes upstream without the owner's
say-so. Survey and failure catalog: `2026-10-22-guest-threading-survey.md`.

## 1. Intent

Today the host decides how the guest kernel's dispatcher behaves (survey §4):
who wakes, in what order, what a suspend count means, when an APC runs, what
clock a timeout runs on. This design moves those decisions into a functional
core that models the Xbox 360 kernel's dispatcher as plain data, and makes
the host threads an imperative shell that carries them out, the same way on
Windows and Linux.

What it delivers:

- Wait and signal semantics as the guest kernel defines them, identical on
  Windows and Linux.
- Exact, side-effect-free introspection (the automation channel's threads
  op reads the core).
- One notion of "stopped by the emulator", separate from the guest's suspend
  count, replacing per-subsystem pause handshakes.
- Timeouts and timers on guest time.
- A place for guest exception dispatch (phase 2).

## 2. Goals, non-goals, anti-goals

Goals:

- Every waitable object's state, every wait, every waiter list lives in the
  core. Host primitives carry no guest semantics: each guest thread blocks on
  one private park primitive.
- Pure decisions: every core operation takes the state and an input and
  returns the new state plus the effects (threads to wake, with what status;
  APCs to deliver). Unit tests drive it with no threads and no clock.
- Bisectable migration: each commit builds, runs, and moves one kind of
  object or one rule; the rest keeps working on the old path until moved.

Non-goals (for this design):

- Scheduling guest threads onto 6 host threads like the hardware (decision 2).
  One host thread per guest thread stays; the core decides wakes, the host OS
  still runs ready threads in parallel.
- Cycle-exact preemption, quantum ends, or priority inversion behaviour of the
  hardware scheduler.

Anti-goals:

- No per-title flags or conditionals.
- No behaviour that differs between Windows and Linux.
- No guessing semantics that public documentation or observation doesn't
  support: where the guest kernel's exact rule is unknown, the core follows
  the documented NT rule and says so in a comment.

## 3. Guest semantics the core implements

The Xbox 360 kernel is an NT-family kernel; its dispatcher objects and waits
follow the documented NT dispatcher (public Microsoft WDK documentation for
`KeInitializeEvent`, `KeSetEvent`, `KePulseEvent`, `KeReleaseSemaphore`,
`KeReleaseMutex`, `KeWaitForSingleObject`, `KeWaitForMultipleObjects`,
`KeDelayExecutionThread`, APCs), which titles' observed behaviour matches.

- **Signal state** is a signed count per object. Notification events: 0/1,
  set wakes every waiter and stays set. Synchronization events: set wakes one
  waiter and the wake consumes the signal; if none waits, it stays set.
  Semaphores: count 0..limit; each satisfied wait takes one. Mutants: signaled
  when unowned; a wait by the owner always succeeds and increments the
  recursion; release by the owner decrements, and at 0 the mutant passes to
  the next waiter. Thread objects: signaled when the thread has terminated.
  Timers (phase 1b): notification or synchronization like events.
- **Waits** name 1..64 objects, any or all. Wait-any is satisfied by the first
  signaled object in list order (status = its index); wait-all only when every
  object is signaled at once, and then all are consumed together. A wait that
  can be satisfied at once never blocks.
- **Waiter order**: first come, first served per object (NT queues waiters at
  the tail; priority does not reorder a wait list).
- **Satisfying a wait** has side effects only through the satisfier
  (consuming a synchronization event, decrementing a semaphore, taking a
  mutant); a pulse wakes current waiters as if set, then resets.
- **Abandoned mutants**: when an owning thread terminates, each mutant it
  owns is released and marked abandoned; the next acquirer gets
  `STATUS_ABANDONED_WAIT_n` (or `STATUS_ABANDONED`) and owns it.
- **Timeouts**: none (forever), zero (poll), relative (negative 100 ns
  units), absolute (positive: a system time). All are deadlines on guest time.
  Expiry wakes the waiter with `STATUS_TIMEOUT`.
- **Alertable waits and user APCs**: a user APC queued to a thread in an
  alertable user-mode wait (or found pending when it starts one) ends the wait
  with `STATUS_USER_APC`, and the APCs run before the wait returns. Kernel
  APCs run regardless of alertability (phase 1c: delivered, not discarded).
- **Suspension**: the guest suspend count (KTHREAD `suspend_count`) and the
  emulator's stops are separate inputs; a thread runs only when both allow it.

## 4. The design

### 4.1 Shape: functional core, imperative shell

```
guest export (shim) ──► Shell: lock dispatcher, translate handles/pointers
                              │
                              ▼
                   Core (pure): state + op ─► state' + effects
                              │
                              ▼
          Shell: apply effects (write guest headers, unpark threads),
                 unlock; a blocked caller parks on its own primitive
```

- **Core** (`kernel/dispatch/`): `Dispatcher` holding `ObjectState` and
  `ThreadState` tables keyed by small ids. Operations:
  `Signal`, `Pulse`, `Reset`, `ReleaseSemaphore`, `ReleaseMutant`,
  `BeginWait`, `Expire(now)`, `QueueApc`, `ThreadTerminated`, `Stop`/`Start`
  (emulator), `Suspend`/`Resume` (guest), `Query`. Each returns an `Effects`
  value: wakes `(thread, status)`, guest-header writes `(object, signal
  state)`, the next deadline. No host calls, no clock reads, no allocation
  outside its own containers.
- **Shell** (`kernel/dispatch/` + the existing `X*` objects): one dispatcher
  lock (a leaf below `global_critical_region`, §4.6), handle and pointer
  translation, guest header mirroring, the park primitive per thread, a
  deadline timer that calls `Expire` on guest time.

### 4.2 Objects

Each `XEvent`, `XSemaphore`, `XMutant`, `XThread`, `XTimer` owns an `ObjectId`
in the core instead of a host primitive (during migration, one kind at a time;
§6). Guest-allocated objects keep the stash binding, but the stash maps to an
`ObjectId`; the guest header's `signal_state` is written whenever the core
changes it (write-through), and guest writes to a header are read into the
core (`SetSignalState`) on every export that receives the object, `Ke*` and
`Nt*` alike.

### 4.3 Waits

`XObject::Wait`, `WaitMultiple`, `SignalAndWait` become one shell function:
lock, `BeginWait`; if the result is immediate, apply effects and return; else
record the deadline, unlock, park; on unpark read the status the waker left.
`SignalAndWait` is one core operation (signal then wait, atomically). The
`WaitScope` records of the threads op are replaced by the core's own thread
state.

### 4.4 Park primitive

One per guest thread: a host event (Win32 auto-reset event; POSIX mutex +
condvar) used only as "you may run now". Parking is alertable to host APCs on
neither platform: APCs are a core input (§4.7), not a host mechanism. A park
has a host timeout only as a safety net for the deadline timer, never as the
semantics.

### 4.5 Time

Deadlines are guest system time (100 ns). The shell keeps one deadline timer:
it sleeps until the earliest deadline converted to host time at the current
time scalar, then calls `Expire(guest_now)`; while the emulator is paused,
guest time stands still and nothing expires.

### 4.6 Locks

- `global_critical_region` stays the outer lock (object table, handle lookup).
- The dispatcher lock is acquired after it, never before; no other lock is
  taken while holding it (it is a leaf), and nothing blocks while holding it.
- Coherence's tracker mutex stays a leaf; the dispatcher lock is never held
  across a memory fault (a fault in a guest header access is taken before the
  lock, by touching the header first).

### 4.7 APCs (phase 1c)

`QueueApc(thread, mode)` marks the APC pending in the core; if the thread is
in an alertable wait (user APC) or any wait (kernel APC), the core wakes it
with the right status. Before blocking, `BeginWait` checks pending APCs. The
shell then runs the APCs on the woken thread as today
(`xeProcessUserApcs`), and kernel APCs too.

### 4.8 Emulator stops and guest suspension (phase 1d)

The core keeps `guest_suspend_count` and `emulator_stops` per thread. A thread
"may run" only when both are zero; the shell parks a thread that may not run
at its next kernel call or safe point (`StepToGuestSafePoint`), and host
suspend remains only as the mechanism for stopping a thread that is running
guest code, never as the record. `Emulator::Pause` becomes one core stop over
every guest thread plus the guest-code host workers (audio callbacks, vblank),
which take the same stop at their own loop points.

### 4.9 Interrupts and DPCs (phase 1e)

Graphics interrupts and DPCs are queued to a processor in the core and run on
a guest thread context with that processor's identity and IRQL 2, in queue
order, before the processor's next guest thread runs. The vblank host thread
only queues. This also fixes "active CPU changed" (survey §2.3).

### 4.10 Priorities (phase 1f)

Base and current priority, boosts on wake (the satisfier's increment), and
decay on quantum end live in the core; the host thread priority follows the
core's current priority through the existing 5-bucket map (when
`ignore_thread_priorities` is off). `KeQueryBasePriorityThread` and friends
report guest values.

### 4.11 Exceptions (phase 2)

Guest exception dispatch through the title's `.pdata` function table:
`RtlRaiseException`, the dispatch of a host-caught access violation or trap to
the guest, `__C_specific_handler` scope tables, `RtlUnwind`, unhandled
exception filters. Designed in its own spec on top of the core's thread
state; out of scope here except that the core's thread state is what it
builds on.

## 5. Constraints

- Clean room: semantics from public Microsoft documentation and observed
  title behaviour only. No XDK material.
- Cross-platform: the same core on every host; the shell's only
  platform-specific part is the park primitive.
- Performance: waits that are satisfied at once take one uncontended lock and
  no syscall; a blocking wait costs one park. Titles wait thousands of times a
  second.
- Every commit builds and runs; the old host-primitive path remains for kinds
  not yet migrated.

## 6. Commit sequence (phase 1)

1. Core types and operations for events, semaphores, mutants and thread
   objects, with unit tests. Not used yet.
2. Park primitive per XThread and the shell's wait/wake plumbing, unused.
3. Events through the core (`XEvent`, set/pulse/reset, single and multiple
   waits whose objects are all core-backed; mixed waits use the old path).
4. Semaphores through the core.
5. Mutants through the core: ownership, recursion, abandonment;
   `KeInitializeMutant`, `KeReleaseMutant`, guest `KMUTANT` headers.
6. Thread objects through the core (signaled on termination; abandon owned
   mutants). Remove the old host-primitive wait path.
7. Guest-time deadlines (relative and absolute timeouts, delays).
8. APCs through the core (phase 1c).
9. Emulator stops and guest suspension (phase 1d).
10. Timers through the core (phase 1b): `KeSetTimer(Ex)`, `KeCancelTimer`,
    DPCs on expiry.
11. Interrupts and DPCs (phase 1e).
12. Priorities and boosts (phase 1f).

Each step: unit tests for the core change; an integration build with the
step; a headless smoke run over the acceptance and regression titles
(boot, reach a frame, no crash, no new stall), A/B against the integration
build without it.

## 7. Risks

- **Throughput**: guest code that hammers events and critical sections. Keep
  the uncontended path syscall-free; measure with `game_bench` on 454107D9 and
  the regression set before and after step 3.
- **Mixed waits during migration** (an event and a mutant in one wait-all):
  they stay on the old path until step 6.
- **Titles that depend on host behaviour we replace** (e.g. relying on a
  host-pulse race): the A/B runs catch it; the core follows the documented
  rule.
- **Guest headers written by titles directly** without an export: still not
  observed (as today) until a fault-based watch exists; out of scope.

## 8. Validation

- Unit tests: every rule in §3 as a core test, with hand-built states.
- Runtime: per step, the headless smoke over the acceptance titles (survey §7)
  and the probe regression set, on the integration build, A/B with and
  without the step; threads snapshots on any stall.
- Platforms: Windows (D3D12) every step; Linux (Vulkan, the Arc server image)
  at steps 3, 6 and 9.
