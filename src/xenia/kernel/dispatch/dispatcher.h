/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_DISPATCH_DISPATCHER_H_
#define XENIA_KERNEL_DISPATCH_DISPATCHER_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace xe {
namespace kernel {
namespace dispatch {

// The guest kernel's dispatcher as plain data: waitable objects, their signal
// state and waiter queues, and the threads waiting on them. Every operation
// takes the state and an input and returns what happened (Effects); it never
// blocks, reads a clock or calls the host. The shell (dispatch/shell.h) holds
// the lock, writes guest headers and wakes host threads from the Effects.
//
// The rules are the NT dispatcher's, as Microsoft's public documentation of
// KeSetEvent, KePulseEvent, KeReleaseSemaphore, KeReleaseMutex and
// KeWaitForMultipleObjects describes them, which Xbox 360 titles follow.

using ObjectId = uint32_t;
using ThreadId = uint32_t;
constexpr ObjectId kNoObject = 0;
constexpr ThreadId kNoThread = 0;

// Wait results, with the NT status values the guest sees.
constexpr uint32_t kStatusWait0 = 0x00000000;
constexpr uint32_t kStatusAbandonedWait0 = 0x00000080;
constexpr uint32_t kStatusUserApc = 0x000000C0;
constexpr uint32_t kStatusTimeout = 0x00000102;

// At most this many objects in one wait, as the guest kernel allows.
constexpr size_t kMaxWaitObjects = 64;

// The highest suspend count (MAXIMUM_SUSPEND_COUNT in the public WDK headers).
constexpr uint32_t kMaxSuspendCount = 0x7F;

enum class ObjectKind : uint8_t {
  // Set wakes every waiter and stays set until reset.
  kNotificationEvent,
  // Set wakes one waiter, whose wake clears it; with none, it stays set.
  kSynchronizationEvent,
  // A count from 0 to its limit; each satisfied wait takes one.
  kSemaphore,
  // Signaled when unowned. Signal state 1 - recursion: 1 free, 0 owned once,
  // -1 owned twice by the same thread, and so on.
  kMutant,
  // Signaled once its thread has terminated.
  kThread,
  // Signals like a notification event when it expires.
  kNotificationTimer,
  // Signals like a synchronization event when it expires.
  kSynchronizationTimer,
};

enum class WaitType : uint8_t { kAny, kAll };

struct Wake {
  ThreadId thread;
  uint32_t status;
  bool operator==(const Wake&) const = default;
};

// The final signal state of an object an operation changed, for the guest
// header (X_DISPATCH_HEADER::signal_state).
struct SignalWrite {
  ObjectId object;
  int32_t signal_state;
  bool operator==(const SignalWrite&) const = default;
};

// What NtSetTimerEx asks for: a routine queued to thread as a user-mode APC
// each time the timer expires.
struct TimerRoutine {
  ThreadId thread;
  uint32_t routine;
  uint32_t argument;
  bool operator==(const TimerRoutine&) const = default;
};

// A routine to queue: the timer, its TimerRoutine and the guest system time
// it expired at (the routine's time arguments).
struct TimerApc {
  ObjectId timer;
  ThreadId thread;
  uint32_t routine;
  uint32_t argument;
  uint64_t time;
  bool operator==(const TimerApc&) const = default;
};

// A timer's routine, queued to thread and not yet taken, that is withdrawn.
struct WithdrawnRoutine {
  ThreadId thread;
  ObjectId timer;
  bool operator==(const WithdrawnRoutine&) const = default;
};

struct Effects {
  // In the order the threads were satisfied.
  std::vector<Wake> wakes;
  std::vector<SignalWrite> writes;
  // Timer routines to queue, in expiry order.
  std::vector<TimerApc> timer_apcs;
  // Queued routines the shell drops: their timer was cancelled, re-armed or
  // destroyed, or their thread terminated.
  std::vector<WithdrawnRoutine> withdrawn_routines;
};

struct WaitRequest {
  std::vector<ObjectId> objects;
  WaitType type = WaitType::kAny;
  // Guest system time (100 ns units) when the wait times out; none: never.
  // A deadline at or before now polls: the wait never blocks.
  std::optional<uint64_t> deadline;
  bool alertable = false;
};

struct WaitResult {
  // True if the thread must park until a Wake names it.
  bool blocked = false;
  // The status when not blocked.
  uint32_t status = kStatusWait0;
  Effects effects;
};

struct ObjectInfo {
  ObjectKind kind;
  int32_t signal_state;
  int32_t limit;
  ThreadId owner;
  bool abandoned;
  size_t waiters;
};

struct ThreadInfo {
  bool waiting;
  WaitType type;
  std::vector<ObjectId> objects;
  std::optional<uint64_t> deadline;
  bool alertable;
  bool user_apc_pending;
  uint32_t suspend_count;
  // Waiting, but set aside while suspended: out of every waiter queue.
  bool held;
};

struct TimerInfo {
  bool armed;
  uint64_t due;     // guest system time, 100 ns
  uint64_t period;  // 100 ns; 0: once
  std::optional<TimerRoutine> routine;
};

class Dispatcher {
 public:
  // Objects. signal_state is the initial state: an event's 0/1, a semaphore's
  // count (with limit), a mutant's 1 (free) or 0 (owned by owner).
  ObjectId NewEvent(bool notification, bool signaled);
  ObjectId NewSemaphore(int32_t count, int32_t limit);
  ObjectId NewMutant(ThreadId owner);
  // The object a thread's handle waits on; signaled at ThreadTerminated.
  ObjectId NewThreadObject(ThreadId thread);
  ObjectId NewTimer(bool notification);
  // An object nobody waits on any more (its waiters are woken by now).
  Effects DestroyObject(ObjectId object);

  ThreadId NewThread();
  void DestroyThread(ThreadId thread);

  // Events. previous gets the state before the call.
  Effects SetEvent(ObjectId event, int32_t* previous = nullptr);
  // Wakes the current waiters as Set would, then resets.
  Effects PulseEvent(ObjectId event, int32_t* previous = nullptr);
  Effects ResetEvent(ObjectId event, int32_t* previous = nullptr);

  // Semaphores. A release past the limit changes nothing and sets *exceeded.
  Effects ReleaseSemaphore(ObjectId semaphore, int32_t count,
                           int32_t* previous = nullptr,
                           bool* exceeded = nullptr);

  // Mutants. Only the owner may release (else *not_owned, nothing changes).
  // abandon releases it whatever the recursion, marked abandoned.
  Effects ReleaseMutant(ObjectId mutant, ThreadId caller, bool abandon = false,
                        int32_t* previous = nullptr, bool* not_owned = nullptr);

  // A guest write to an object's header (signal state), read back in.
  Effects SetSignalState(ObjectId object, int32_t signal_state);

  // Timers. Arms timer to expire at due, then every period (0: once).
  // Re-arming replaces the old due time; either way the signal state is
  // cleared. *was_armed gets whether it was armed before.
  Effects SetTimer(ObjectId timer, uint64_t due, uint64_t period,
                   std::optional<TimerRoutine> routine,
                   bool* was_armed = nullptr);
  // Disarms; the signal state is unchanged. *was_armed gets whether it was
  // armed.
  Effects CancelTimer(ObjectId timer, bool* was_armed = nullptr);
  // The guest clock interrupt: expires every armed timer due at or before
  // now, in due order, ties in the order they were armed. A timer has one
  // routine queued at a time: until it is taken, an expiry queues none.
  Effects Tick(uint64_t now);
  std::optional<TimerInfo> QueryTimer(ObjectId timer) const;
  // The shell handed these routines to their threads.
  void TimerRoutinesTaken(std::span<const TimerApc> apcs);

  // Waits. A thread waits on at most one request at a time. now is the
  // guest system time, for a deadline that has passed already. A suspended
  // thread's wait begins held.
  WaitResult BeginWait(ThreadId thread, const WaitRequest& request,
                       uint64_t now);
  // Wakes every thread whose deadline is at or before now with a timeout.
  Effects Expire(uint64_t now);
  // The earliest deadline among waiting threads.
  std::optional<uint64_t> NextDeadline() const;

  // A user-mode APC was queued to thread: it ends an alertable wait with
  // kStatusUserApc, or stays pending so the next alertable wait returns at
  // once. UserApcsDelivered clears it once the shell has run them.
  Effects QueueUserApc(ThreadId thread);
  void UserApcsDelivered(ThreadId thread);

  // The guest suspend count. A suspended thread doesn't run: a wait it is in
  // is held (it takes no signal, doesn't time out and no APC ends it), and
  // the last Resume puts it back at the end of its queues, so it then ends
  // as it would have begun at that moment. Suspend past kMaxSuspendCount
  // changes nothing and sets *exceeded. previous gets the count before.
  Effects Suspend(ThreadId thread, uint32_t* previous = nullptr,
                  bool* exceeded = nullptr);
  Effects Resume(ThreadId thread, uint64_t now, uint32_t* previous = nullptr);
  // Where a suspended thread that isn't in a wait stops: blocked until the
  // last Resume, which wakes it with kStatusWait0. Doesn't block when the
  // thread isn't suspended.
  WaitResult BeginHold(ThreadId thread);

  // The thread ended: its thread object is signaled, and each mutant it owns
  // is released and marked abandoned. A wait it was in is dropped.
  Effects ThreadTerminated(ThreadId thread);

  std::optional<ObjectInfo> Query(ObjectId object) const;
  std::optional<ThreadInfo> QueryThread(ThreadId thread) const;

 private:
  struct Object {
    ObjectKind kind;
    int32_t signal_state = 0;
    int32_t limit = 0;
    ThreadId owner = kNoThread;
    bool abandoned = false;
    // For a thread object, its thread.
    ThreadId thread = kNoThread;
    std::deque<ThreadId> waiters;
    // For a timer.
    bool armed = false;
    uint64_t due = 0;
    uint64_t period = 0;
    std::optional<TimerRoutine> routine;
    // Its routine is queued to routine->thread and not yet taken.
    bool routine_queued = false;
    // The order timers were armed in, to break ties in due time.
    uint64_t sequence = 0;
  };
  struct Thread {
    bool waiting = false;
    WaitType type = WaitType::kAny;
    std::vector<ObjectId> objects;
    std::optional<uint64_t> deadline;
    bool alertable = false;
    bool user_apc_pending = false;
    uint32_t suspend_count = 0;
    bool held = false;
    // In BeginHold's wait rather than the guest's.
    bool holding = false;
  };

  ObjectId AddObject(Object object);
  // Whether object would satisfy a wait by thread now.
  bool SignaledFor(const Object& object, ThreadId thread) const;
  // Takes object for thread: consumes it as its kind does. Returns true if it
  // was abandoned (the acquirer learns that once).
  bool Consume(ObjectId id, ThreadId thread, Effects& effects);
  // The status a thread's wait would end with now, if it can be satisfied,
  // consuming what it takes.
  std::optional<uint32_t> TrySatisfy(ThreadId id, Thread& thread,
                                     Effects& effects);
  // How the thread's request ends now without blocking, if it does: a pending
  // user APC, satisfied, or past its deadline.
  std::optional<uint32_t> TryFinish(ThreadId id, Thread& thread, uint64_t now,
                                    Effects& effects);
  // Joins (or leaves) the waiter queues of the thread's objects.
  void Enqueue(ThreadId id, const Thread& thread);
  void Dequeue(ThreadId id, const Thread& thread);
  // Satisfies the waiters of object that can be, in queue order.
  void WakeWaiters(ObjectId object, Effects& effects);
  void EndWait(ThreadId id, Thread& thread, uint32_t status, Effects& effects);
  void NoteSignal(ObjectId id, Effects& effects);
  // Forgets the routines of the timers that target thread.
  void DropTimerRoutines(ThreadId thread, Effects& effects);
  // Withdraws timer's queued routine, if it has one.
  static void WithdrawRoutine(ObjectId id, Object& timer, Effects& effects);

  std::unordered_map<ObjectId, Object> objects_;
  std::unordered_map<ThreadId, Thread> threads_;
  ObjectId next_object_ = 1;
  ThreadId next_thread_ = 1;
  uint64_t next_sequence_ = 1;
};

}  // namespace dispatch
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_DISPATCH_DISPATCHER_H_
