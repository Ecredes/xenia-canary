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

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace dispatch {

namespace {

// A parked waiter with a deadline looks at the guest clock again at least
// this often: guest time can stop (a pause) or change speed.
constexpr int64_t kMaxParkMs = 1000;

}  // namespace

Shell::Shell() = default;

Shell::~Shell() = default;

void Shell::Bind(ObjectId id, XObject* object) {
  std::lock_guard<std::mutex> lock(mutex_);
  objects_[id] = object;
}

void Shell::Destroy(ObjectId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  objects_.erase(id);
  core_.DestroyObject(id);
}

void Shell::AddWaiter(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiter.shell == this) {
    return;
  }
  if (!waiter.park) {
    waiter.park = xe::threading::Event::CreateAutoResetEvent(false);
  }
  waiter.id = core_.NewThread();
  waiter.shell = this;
  waiters_[waiter.id] = &waiter;
}

void Shell::RemoveWaiter(Waiter& waiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (waiter.shell != this) {
    return;
  }
  waiters_.erase(waiter.id);
  core_.DestroyThread(waiter.id);
  waiter.shell = nullptr;
  waiter.id = kNoThread;
}

Waiter& Shell::CurrentWaiter() {
  if (XThread* thread = XThread::TryGetCurrentThread()) {
    Waiter& waiter = thread->waiter();
    AddWaiter(waiter);
    return waiter;
  }
  // A host thread that isn't an XThread (the UI, a worker) waiting on a
  // guest object. Its waiter lives as long as the thread.
  thread_local Waiter host_waiter;
  AddWaiter(host_waiter);
  return host_waiter;
}

const XThread* Shell::ThreadOf(ThreadId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto waiter = waiters_.find(id);
  return waiter != waiters_.end() ? waiter->second->thread : nullptr;
}

void Shell::ApplyLocked(const Effects& effects) {
  for (const SignalWrite& write : effects.writes) {
    auto object = objects_.find(write.object);
    if (object != objects_.end()) {
      object->second->WriteSignalState(write.signal_state);
    }
  }
  for (const Wake& wake : effects.wakes) {
    auto waiter = waiters_.find(wake.thread);
    if (waiter == waiters_.end()) {
      continue;
    }
    waiter->second->status = wake.status;
    waiter->second->park->Set();
  }
}

uint32_t Shell::Wait(const WaitRequest& request) {
  Waiter& waiter = CurrentWaiter();
  std::unique_lock<std::mutex> lock(mutex_);
  return WaitLocked(waiter, request, lock);
}

uint32_t Shell::WaitLocked(Waiter& waiter, const WaitRequest& request,
                           std::unique_lock<std::mutex>& lock) {
  WaitResult result = core_.BeginWait(waiter.id, request, GuestNow());
  ApplyLocked(result.effects);
  if (!result.blocked) {
    return result.status;
  }
  while (true) {
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
    if (request.deadline) {
      uint64_t now = GuestNow();
      double remaining_ms =
          now >= *request.deadline
              ? 0.0
              : double(*request.deadline - now) / 10000.0 /
                    std::max(Clock::guest_time_scalar(), 1e-6);
      timeout = std::chrono::milliseconds(
          std::clamp<int64_t>(int64_t(std::ceil(remaining_ms)), 1, kMaxParkMs));
    }
    lock.unlock();
    // Alertable, so host APCs (timer completion routines) still run; the
    // guest's own APCs come through the core.
    xe::threading::Wait(waiter.park.get(), true, timeout);
    lock.lock();
    std::optional<ThreadInfo> info = core_.QueryThread(waiter.id);
    if (!info || !info->waiting) {
      // Satisfied, timed out or ended by an APC: the waker left the status.
      waiter.park->Reset();
      return waiter.status;
    }
    if (request.deadline && GuestNow() >= *request.deadline) {
      ApplyLocked(core_.Expire(GuestNow()));
    }
  }
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
