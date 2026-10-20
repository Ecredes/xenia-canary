/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TITLE_LAUNCH_H_
#define XENIA_APP_TITLE_LAUNCH_H_

#include <filesystem>
#include <string>

#include "xenia/xbox.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xe {
namespace app {

enum class TitleLaunchError {
  kNone,
  kTitleOpen,
  kNotFound,
  kArchive,
  kLaunchFailed,
};

struct TitleLaunchResult {
  X_STATUS status = X_STATUS_SUCCESS;
  TitleLaunchError error = TitleLaunchError::kNone;
  // What to tell the user on failure.
  std::string message;
};

// Launches a title from a host path, without any UI, and remembers the path
// for a title relaunch.
TitleLaunchResult LaunchTitle(Emulator& emulator,
                              const std::filesystem::path& path_to_file);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_TITLE_LAUNCH_H_
