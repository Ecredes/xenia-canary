/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_input_driver.h"

#include <utility>

namespace xe {
namespace hid {
namespace remote {

RemoteInputDriver::RemoteInputDriver(xe::ui::Window* window,
                                     size_t window_z_order,
                                     std::shared_ptr<RemotePads> pads)
    : InputDriver(window, window_z_order), pads_(std::move(pads)) {}

}  // namespace remote
}  // namespace hid
}  // namespace xe
