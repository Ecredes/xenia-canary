/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <optional>

#include "xenia/base/logging.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(dispatch_object());
  // A notification timer (0) stays signaled until set again; a
  // synchronization timer (1) releases one waiter.
  assert_true(timer_type <= 1);
  bool notification = timer_type == 0;
  BindDispatchObject(kernel_state()->dispatch()->Read(
      [notification](dispatch::Dispatcher& core) {
        return core.NewTimer(notification);
      }));
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  // Guest time throughout: the clock interrupt expires it.
  uint64_t timeout = uint64_t(due_time);
  uint64_t due = *dispatch::Shell::DeadlineFromTimeout(&timeout);
  uint64_t period = uint64_t(period_ms) * 10000;
  // The routine runs as an APC on the thread that set the timer.
  std::optional<dispatch::TimerRoutine> timer_routine;
  XThread* thread = XThread::TryGetCurrentThread();
  if (routine && thread) {
    timer_routine =
        dispatch::TimerRoutine{thread->waiter().id, routine, routine_arg};
  }
  kernel_state()->dispatch()->Run([&](dispatch::Dispatcher& core) {
    return core.SetTimer(dispatch_object(), due, period, timer_routine);
  });

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XTimer::Cancel(bool* was_signaled) {
  bool signaled =
      kernel_state()->dispatch()->Read([this](dispatch::Dispatcher& core) {
        std::optional<dispatch::ObjectInfo> info =
            core.Query(dispatch_object());
        core.CancelTimer(dispatch_object());
        return info && info->signal_state != 0;
      });
  if (was_signaled) {
    *was_signaled = signaled;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
