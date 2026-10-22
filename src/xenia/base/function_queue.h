/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_FUNCTION_QUEUE_H_
#define XENIA_BASE_FUNCTION_QUEUE_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <utility>

namespace xe {

// Functions pushed from any thread, run in order by one consumer thread.
class FunctionQueue {
 public:
  void Push(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(fn));
    has_pending_.store(true, std::memory_order_release);
  }

  // Lock-free, so a consumer can poll it while spinning.
  bool empty() const { return !has_pending_.load(std::memory_order_acquire); }

  // Runs each function without the lock held, so it may push more.
  void RunAll() {
    while (true) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
          return;
        }
        fn = std::move(queue_.front());
        queue_.pop();
        has_pending_.store(!queue_.empty(), std::memory_order_release);
      }
      fn();
    }
  }

 private:
  std::mutex mutex_;
  std::queue<std::function<void()>> queue_;
  std::atomic<bool> has_pending_ = false;
};

}  // namespace xe

#endif  // XENIA_BASE_FUNCTION_QUEUE_H_
