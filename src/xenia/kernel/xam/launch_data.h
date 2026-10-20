/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_LAUNCH_DATA_H_
#define XENIA_KERNEL_XAM_LAUNCH_DATA_H_

#include <filesystem>
#include <system_error>

namespace xe {
namespace kernel {
namespace xam {

// Whether launch data a title saved for its relaunch applies to this launch:
// saved_host_path is the title it belongs to, target the one named on the
// command line (empty if none). Data for another title must not change how
// the target launches.
inline bool LaunchDataAppliesTo(const std::filesystem::path& saved_host_path,
                                const std::filesystem::path& target) {
  if (saved_host_path.empty()) {
    return false;
  }
  if (target.empty()) {
    return true;
  }
  std::error_code ec;
  if (std::filesystem::equivalent(saved_host_path, target, ec)) {
    return true;
  }
  // Not both existing: compare the paths themselves.
  std::filesystem::path saved_absolute =
      std::filesystem::absolute(saved_host_path, ec);
  std::filesystem::path target_absolute = std::filesystem::absolute(target, ec);
  return saved_absolute.lexically_normal() ==
         target_absolute.lexically_normal();
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_LAUNCH_DATA_H_
