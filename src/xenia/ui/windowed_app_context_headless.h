/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_CONTEXT_HEADLESS_H_
#define XENIA_UI_WINDOWED_APP_CONTEXT_HEADLESS_H_

#include <condition_variable>
#include <mutex>

#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace ui {

// A UI thread loop without a windowing system, for running an app that creates
// no windows (--headless).
class HeadlessWindowedAppContext final : public WindowedAppContext {
 public:
  HeadlessWindowedAppContext() = default;

  void NotifyUILoopOfPendingFunctions() override;

  void PlatformQuitFromUIThread() override;

  void RunMainLoop();

 private:
  std::mutex loop_mutex_;
  std::condition_variable loop_cv_;
  bool functions_pending_ = false;
  bool quit_ = false;
};

// Whether the command line asks for --headless, checked before any windowing
// system is initialized (cvars aren't parsed yet then).
bool IsHeadlessRequested(int argc, const char* const* argv);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_HEADLESS_H_
