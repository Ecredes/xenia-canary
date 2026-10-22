/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/dispatch/shell.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/kernel/xobject.h"

namespace xe {
namespace kernel {
namespace dispatch {

namespace {

// A parked waiter with a deadline looks at the guest clock again at least
// this often: guest time can stop (a pause) or change speed.
constexpr int64_t kMaxParkMs = 1000;

}  // namespace

Shell::Shell()
    : serial_([] {
        static std::atomic<uint64_t> next_serial{1};
        return next_serial++;
      }()) {}

Shell::~Shell() = default;

void Shell::Bind(ObjectId id, XObject* object) {
  std::lock_guard<std::mutex> lock(mutex_);
  objects_[id] = object;
}

void Shell::Destroy(ObjectId id) {
  Unparks unparks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    objects_.erase(id);
    ApplyLocked(core_.DestroyObject(id), unparks);
  }
  Unpark(unparks);
}

void Shell::AddWaiter(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiter.shell == serial_) {
    return;
  }
  if (!waiter.park) {
    waiter.park = xe::threading::Event::CreateAutoResetEvent(false);
  }
  waiter.id = core_.NewThread();
  waiter.shell = serial_;
  // A host thread's waiter may come from an earlier shell, with its routines.
  waiter.timer_apcs.clear();
  waiters_[waiter.id] = &waiter;
}

void Shell::RemoveWaiter(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiter.shell != serial_) {
    return;
  }
  waiters_.erase(waiter.id);
  core_.DestroyThread(waiter.id);
  waiter.timer_apcs.clear();
  waiter.shell = 0;
  waiter.id = kNoThread;
}

Waiter& Shell::CurrentWaiter() {
  // waiter.shell is written only before the thread starts or by the thread
  // itself, so it can be read without the lock.
  if (Waiter* waiter = waiter_source_ ? waiter_source_() : nullptr) {
    if (waiter->shell != serial_) {
      AddWaiter(*waiter);
    }
    return *waiter;
  }
  // A host thread that isn't an XThread (the UI, a worker) waiting on a
  // guest object. Its waiter lives as long as the thread.
  thread_local Waiter host_waiter;
  if (host_waiter.shell != serial_) {
    AddWaiter(host_waiter);
  }
  return host_waiter;
}

const XThread* Shell::ThreadOf(ThreadId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto waiter = waiters_.find(id);
  return waiter != waiters_.end() ? waiter->second->thread : nullptr;
}

void Shell::Unpark(const Unparks& unparks) {
  for (const auto& park : unparks) {
    park->Set();
  }
}

void Shell::ApplyLocked(const Effects& effects, Unparks& unparks) {
  for (const SignalWrite& write : effects.writes) {
    auto object = objects_.find(write.object);
    if (object != objects_.end()) {
      object->second->WriteSignalState(write.signal_state);
    }
  }
  for (const WithdrawnRoutine& withdrawn : effects.withdrawn_routines) {
    auto waiter = waiters_.find(withdrawn.thread);
    if (waiter != waiters_.end()) {
      std::erase_if(waiter->second->timer_apcs, [&](const TimerApc& apc) {
        return apc.timer == withdrawn.timer;
      });
    }
  }
  for (const TimerApc& apc : effects.timer_apcs) {
    auto waiter = waiters_.find(apc.thread);
    if (waiter != waiters_.end()) {
      waiter->second->timer_apcs.push_back(apc);
    } else {
      // Nobody to take it: its timer may queue the next one.
      core_.TimerRoutinesTaken({&apc, 1});
    }
  }
  for (const Wake& wake : effects.wakes) {
    auto waiter = waiters_.find(wake.thread);
    if (waiter == waiters_.end()) {
      continue;
    }
    waiter->second->status.store(wake.status, std::memory_order_relaxed);
    waiter->second->woken.store(true, std::memory_order_release);
    unparks.push_back(waiter->second->park);
  }
}

uint32_t Shell::Wait(const WaitRequest& request) {
  Waiter& waiter = CurrentWaiter();
  Unparks unparks;
  std::unique_lock<std::mutex> lock(mutex_);
  return WaitLocked(waiter, request, lock, unparks);
}

uint32_t Shell::WaitLocked(Waiter& waiter, const WaitRequest& request,
                           std::unique_lock<std::mutex>& lock,
                           Unparks& unparks) {
  PrepareLocked(waiter);
  WaitResult result = core_.BeginWait(waiter.id, request, GuestNow());
  return ParkLocked(waiter, result, request.deadline, lock, unparks);
}

void Shell::PrepareLocked(Waiter& waiter) {
  // A set left over from an earlier wait's waker; the flag is the truth.
  waiter.park->Reset();
  waiter.woken.store(false, std::memory_order_relaxed);
}

uint32_t Shell::ParkLocked(Waiter& waiter, const WaitResult& result,
                           std::optional<uint64_t> deadline,
                           std::unique_lock<std::mutex>& lock,
                           Unparks& unparks) {
  ApplyLocked(result.effects, unparks);
  if (!result.blocked) {
    lock.unlock();
    Unpark(unparks);
    return result.status;
  }
  // Kept alive here, should the waiter go with its thread object while this
  // thread is parked (a thread created suspended and never resumed).
  std::shared_ptr<xe::threading::Event> park = waiter.park;
  // Once the deadline has been handed to the core, only a wake ends the wait
  // (a held one waits for its Resume).
  bool expired = false;
  while (true) {
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
    if (deadline && expired) {
      timeout = std::chrono::milliseconds(kMaxParkMs);
    } else if (deadline) {
      uint64_t now = GuestNow();
      double remaining_ms =
          now >= *deadline ? 0.0
                           : double(*deadline - now) / 10000.0 /
                                 std::max(Clock::guest_time_scalar(), 1e-6);
      timeout = std::chrono::milliseconds(
          std::clamp<int64_t>(int64_t(std::ceil(remaining_ms)), 1, kMaxParkMs));
    }
    lock.unlock();
    Unpark(unparks);
    unparks.clear();
    // Not alertable: guest APCs come through the core, as a wake.
    xe::threading::Wait(park.get(), false, timeout);
    if (waiter.woken.load(std::memory_order_acquire)) {
      return waiter.status.load(std::memory_order_relaxed);
    }
    lock.lock();
    if (!waiter.woken.load(std::memory_order_relaxed) && deadline &&
        GuestNow() >= *deadline) {
      ApplyLocked(core_.Expire(GuestNow()), unparks);
      expired = true;
    }
    if (waiter.woken.load(std::memory_order_relaxed)) {
      lock.unlock();
      Unpark(unparks);
      return waiter.status.load(std::memory_order_relaxed);
    }
  }
}

std::vector<TimerApc> Shell::TakeTimerApcs(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TimerApc> apcs = std::exchange(waiter.timer_apcs, {});
  core_.TimerRoutinesTaken(apcs);
  return apcs;
}

void Shell::UserApcsDelivered(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiter.timer_apcs.empty()) {
    core_.UserApcsDelivered(waiter.id);
  }
}

uint32_t Shell::Suspend(Waiter& target, bool* exceeded) {
  Waiter& self = CurrentWaiter();
  uint32_t previous = 0;
  bool over = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    core_.Suspend(target.id, &previous, &over);
    if (!over && target.suspend_count) {
      *target.suspend_count = uint8_t(previous + 1);
    }
    if (!over && previous == 0 && &target != &self && target.host) {
      std::optional<ThreadInfo> info = core_.QueryThread(target.id);
      if (info && !info->waiting) {
        // Running: stop it where it is. With the lock held here, it can't
        // be stopped holding it.
        target.host->Suspend();
        target.host_suspended = true;
      }
    }
  }
  if (exceeded) {
    *exceeded = over;
  }
  if (!over && &target == &self) {
    Hold();
  }
  return previous;
}

uint32_t Shell::Resume(Waiter& target) {
  uint32_t previous = 0;
  Unparks unparks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyLocked(core_.Resume(target.id, GuestNow(), &previous), unparks);
    if (previous && target.suspend_count) {
      *target.suspend_count = uint8_t(previous - 1);
    }
    if (previous == 1 && target.host_suspended) {
      target.host->Resume();
      target.host_suspended = false;
    }
  }
  Unpark(unparks);
  return previous;
}

void Shell::Hold() {
  Waiter& waiter = CurrentWaiter();
  Unparks unparks;
  std::unique_lock<std::mutex> lock(mutex_);
  PrepareLocked(waiter);
  WaitResult result = core_.BeginHold(waiter.id);
  ParkLocked(waiter, result, std::nullopt, lock, unparks);
}

uint64_t Shell::GuestNow() { return Clock::QueryGuestSystemTime(); }

std::optional<uint64_t> Shell::DeadlineFromTimeout(
    const uint64_t* opt_timeout) {
  if (!opt_timeout) {
    return std::nullopt;
  }
  int64_t timeout = int64_t(*opt_timeout);
  uint64_t now = GuestNow();
  if (timeout < 0) {
    return now + uint64_t(-timeout);
  }
  if (timeout == 0) {
    return now;
  }
  return uint64_t(timeout);
}

}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
