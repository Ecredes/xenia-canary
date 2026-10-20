/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_headless.h"

#include <string_view>

namespace xe {
namespace ui {

void HeadlessWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  {
    std::lock_guard<std::mutex> loop_lock(loop_mutex_);
    functions_pending_ = true;
  }
  loop_cv_.notify_one();
}

void HeadlessWindowedAppContext::PlatformQuitFromUIThread() {
  std::lock_guard<std::mutex> loop_lock(loop_mutex_);
  quit_ = true;
}

void HeadlessWindowedAppContext::RunMainLoop() {
  while (!HasQuitFromUIThread()) {
    {
      std::unique_lock<std::mutex> loop_lock(loop_mutex_);
      loop_cv_.wait(loop_lock, [this] { return functions_pending_ || quit_; });
      if (quit_) {
        break;
      }
      functions_pending_ = false;
    }
    ExecutePendingFunctionsFromUIThread();
  }
  // Something other than QuitFromUIThread may have ended the loop.
  QuitFromUIThread();
}

bool IsHeadlessRequested(int argc, const char* const* argv) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--headless" || arg == "--headless=true") {
      return true;
    }
  }
  return false;
}

}  // namespace ui
}  // namespace xe
