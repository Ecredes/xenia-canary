/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/headless_host.h"

#include <cstdlib>

#include "xenia/base/logging.h"
#include "xenia/headless_policy.h"

namespace xe {

void HeadlessHost::Notify(Notification notification) {
  XELOGI("Notification (user {}): {}{}{}", notification.user_index,
         notification.title, notification.title.empty() ? "" : ": ",
         notification.description);
}

void HeadlessHost::ReportError(ErrorReport report) {
  XELOGE("{}: {}", report.title, report.message);
}

void HeadlessHost::RequestDiscImage(
    std::string prompt, std::function<void(std::filesystem::path)> done) {
  XELOGW("No disc image to swap in without a UI: {}", prompt);
  done({});
}

void HeadlessHost::RequestLifecycle(LifecycleRequest request) {
  int exit_code = HeadlessExitCode(request);
  XELOGI("The title ended itself, exiting with {}", exit_code);
  xe::FlushLog();
  std::quick_exit(exit_code);
}

}  // namespace xe
