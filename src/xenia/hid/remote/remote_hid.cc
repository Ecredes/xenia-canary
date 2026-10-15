/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_hid.h"

#include "xenia/base/cvar.h"
#include "xenia/hid/remote/remote_input_driver.h"

DEFINE_int32(remote_input_port, 0,
             "Local TCP port (127.0.0.1 only) for the remote input driver, "
             "which lets automated test tools drive controllers with exact "
             "analog values and vblank timing. 0 disables it. It is used in "
             "addition to the hid driver and takes priority while a tool has "
             "a controller connected.",
             "HID");

namespace xe {
namespace hid {
namespace remote {

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order,
                                    FrameSource frame_source) {
  if (cvars::remote_input_port <= 0 || cvars::remote_input_port > 65535) {
    return nullptr;
  }
  return std::make_unique<RemoteInputDriver>(window, window_z_order,
                                             uint16_t(cvars::remote_input_port),
                                             std::move(frame_source));
}

}  // namespace remote
}  // namespace hid
}  // namespace xe
