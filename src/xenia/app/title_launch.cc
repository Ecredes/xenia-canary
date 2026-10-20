/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_launch.h"

#include <system_error>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe {
namespace app {

TitleLaunchResult LaunchTitle(Emulator& emulator,
                              const std::filesystem::path& path_to_file) {
  std::error_code ec = {};
  bool title_exists = std::filesystem::exists(path_to_file, ec);

  if (path_to_file.empty() || !title_exists) {
    std::string log_msg =
        fmt::format("Failed to launch title path is {}.",
                    path_to_file.empty() ? "empty" : "invalid");

    if (!path_to_file.empty() && !title_exists) {
      log_msg.append(fmt::format("\nProvided Path: {}", path_to_file));
    }

    if (ec) {
      log_msg.append(fmt::format("\nExtended message info: {} ({:08X})",
                                 ec.message(), ec.value()));
    }

    XELOGE("{}", log_msg);
    return {X_STATUS_NO_SUCH_FILE, TitleLaunchError::kNotFound, log_msg};
  }

  // Prevent crashing the emulator by not loading a game if a game is already
  // loaded.
  if (emulator.is_title_open()) {
    // Terminate the current title and start a new title.
    // if (emulator.TerminateTitle() == X_STATUS_SUCCESS) {
    //   return LaunchTitle(emulator, path_to_file);
    // }

    return {X_STATUS_UNSUCCESSFUL, TitleLaunchError::kTitleOpen, {}};
  }

  auto abs_path = std::filesystem::absolute(path_to_file);

  auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(abs_path.extension()));

  if (extension == ".7z" || extension == ".zip" || extension == ".rar" ||
      extension == ".tar" || extension == ".gz") {
    return {X_STATUS_UNSUCCESSFUL, TitleLaunchError::kArchive,
            "Unsupported format!\n"
            "Xenia does not support running software in an archived format."};
  }

  auto result = emulator.LaunchPath(abs_path);
  if (result) {
    XELOGE("Failed to launch target: {:08X}", result);
    emulator.file_system()->Clear();
    return {
        result, TitleLaunchError::kLaunchFailed,
        "Failed to launch title.\n\nCheck xenia.log for technical details."};
  }

  auto xam = emulator.kernel_state()->GetKernelModule<kernel::xam::XamModule>(
      "xam.xex");
  xam->loader_data().host_path = xe::path_to_utf8(abs_path);

  return {};
}

}  // namespace app
}  // namespace xe
