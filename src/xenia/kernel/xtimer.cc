/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <algorithm>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  if (auto item = wait_item_.lock()) {
    item->Disarm();
  }
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(dispatch_object());
  // A notification timer (0) stays signaled until set again; a
  // synchronization timer (1) releases one waiter.
  assert_true(timer_type <= 1);
  bool manual_reset = timer_type == 0;
  BindDispatchObject(kernel_state()->dispatch()->Read(
      [manual_reset](dispatch::Dispatcher& core) {
        return core.NewEvent(manual_reset, false);
      }));
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using HostClock = xe::threading::TimerQueueWaitItem::clock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  // Fire never takes timer_lock_, so disarming under it can't deadlock.
  if (auto item = wait_item_.lock()) {
    item->Disarm();
  }
  // Setting a timer clears its signal state.
  kernel_state()->dispatch()->Run([this](dispatch::Dispatcher& core) {
    return core.ResetEvent(dispatch_object());
  });

  // The due time is on the guest clock; the timer queue runs on the host's.
  uint64_t now = dispatch::Shell::GuestNow();
  uint64_t remaining_ticks =
      due_time < 0 ? uint64_t(-due_time)
                   : (uint64_t(due_time) > now ? uint64_t(due_time) - now : 0);
  double host_ticks =
      double(remaining_ticks) / std::max(Clock::guest_time_scalar(), 1e-6);
  HostClock::time_point due =
      HostClock::now() +
      std::chrono::duration_cast<HostClock::duration>(
          std::chrono::duration<double, std::ratio<1, 10000000>>(host_ticks));

  callback_thread_ = retain_object(XThread::TryGetCurrentThread());
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  auto fire = [this](void*) { Fire(); };
  if (!period_ms) {
    wait_item_ = xe::threading::QueueTimerOnce(fire, nullptr, due);
  } else {
    wait_item_ = xe::threading::QueueTimerRecurring(
        fire, nullptr, due, std::chrono::milliseconds(period_ms));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
  return X_STATUS_SUCCESS;
}

void XTimer::Fire() {
  kernel_state()->dispatch()->Run([this](dispatch::Dispatcher& core) {
    return core.SetEvent(dispatch_object());
  });
  // The routine runs as an APC on the thread that set the timer, with the
  // guest time it fired at.
  if (callback_routine_ && callback_thread_) {
    callback_thread_->QueueTimerApc(callback_routine_, callback_routine_arg_,
                                    dispatch::Shell::GuestNow());
  }
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (auto item = wait_item_.lock()) {
    item->Disarm();
  }
  wait_item_.reset();
  return X_STATUS_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
