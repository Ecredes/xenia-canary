/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_DISPATCH_SHELL_H_
#define XENIA_KERNEL_DISPATCH_SHELL_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "xenia/base/threading.h"
#include "xenia/kernel/dispatch/dispatcher.h"

namespace xe {
namespace kernel {
class XObject;
class XThread;

namespace dispatch {

// A host thread that waits on guest objects: its dispatcher thread, and the
// one primitive it parks on. The core's decision is woken and status; the
// park only hurries the waiter to look, and may also fire spuriously.
struct Waiter {
  ThreadId id = kNoThread;
  // Shared, so a waker can set it after the lock is gone.
  std::shared_ptr<xe::threading::Event> park;
  std::atomic<uint32_t> status{kStatusWait0};
  // Set (after status) when the core ends the current wait.
  std::atomic<bool> woken{false};
  // The XThread it belongs to; null for a host thread.
  XThread* thread = nullptr;
  // The serial of the shell it is registered with, 0 for none (a host thread
  // may outlive a shell, and a later one may get its address).
  uint64_t shell = 0;
};

// The imperative shell around the dispatcher core: the lock, guest header
// write-through, and parking and waking host threads.
//
// Lock order: global_critical_region before the dispatcher lock, never after;
// the dispatcher lock is a leaf (nothing else is taken while holding it, and
// nothing blocks on it). Callers read the guest headers they name before
// taking it, so a memory-coherence pull doesn't happen under it.
class Shell {
 public:
  Shell();
  ~Shell();

  // Runs op on the core with the lock held and applies its effects. op
  // returns Effects.
  template <typename Op>
  void Run(Op&& op) {
    Unparks unparks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ApplyLocked(op(core_), unparks);
    }
    Unpark(unparks);
  }
  // Runs op on the core with the lock held, for queries and creation.
  template <typename Op>
  auto Read(Op&& op) {
    std::lock_guard<std::mutex> lock(mutex_);
    return op(core_);
  }

  // An object whose signal state is mirrored into its guest header.
  void Bind(ObjectId id, XObject* object);
  // Forgets the object and removes it from the core.
  void Destroy(ObjectId id);

  // Registers waiter with the core (once per shell).
  void AddWaiter(Waiter& waiter);
  void RemoveWaiter(Waiter& waiter);
  // The calling thread's waiter: the source's (an XThread's), or else one
  // for this host thread.
  Waiter& CurrentWaiter();
  // Where CurrentWaiter finds a guest thread's waiter; null when the calling
  // thread has none of its own.
  using WaiterSource = Waiter* (*)();
  void set_waiter_source(WaiterSource source) { waiter_source_ = source; }
  // The XThread a core thread belongs to, or null. Compare, don't
  // dereference: it may be gone by the time it is read.
  const XThread* ThreadOf(ThreadId id);

  // Blocks the calling thread until its wait is satisfied, times out or an
  // APC ends it; returns the NT status (kStatusWait0 + index, ...).
  uint32_t Wait(const WaitRequest& request);
  // signal (on the core, as the caller) then wait, with no gap between.
  template <typename Signal>
  uint32_t SignalAndWait(Signal&& signal, const WaitRequest& request) {
    Waiter& waiter = CurrentWaiter();
    Unparks unparks;
    std::unique_lock<std::mutex> lock(mutex_);
    ApplyLocked(signal(core_, waiter.id), unparks);
    return WaitLocked(waiter, request, lock, unparks);
  }

  // Guest system time, 100 ns units: the clock deadlines run on.
  static uint64_t GuestNow();
  // A deadline from a guest timeout (null: none; negative: relative 100 ns;
  // zero: now, a poll; positive: an absolute guest system time).
  static std::optional<uint64_t> DeadlineFromTimeout(
      const uint64_t* opt_timeout);

 private:
  // Parks to set once the lock is released: a woken thread's first act is
  // to look at its outcome, and it shouldn't find the lock still held.
  using Unparks = std::vector<std::shared_ptr<xe::threading::Event>>;
  static void Unpark(const Unparks& unparks);

  void ApplyLocked(const Effects& effects, Unparks& unparks);
  // Releases lock before returning.
  uint32_t WaitLocked(Waiter& waiter, const WaitRequest& request,
                      std::unique_lock<std::mutex>& lock, Unparks& unparks);

  const uint64_t serial_;
  WaiterSource waiter_source_ = nullptr;
  std::mutex mutex_;
  Dispatcher core_;
  std::unordered_map<ObjectId, XObject*> objects_;
  std::unordered_map<ThreadId, Waiter*> waiters_;
};

}  // namespace dispatch
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_DISPATCH_SHELL_H_
