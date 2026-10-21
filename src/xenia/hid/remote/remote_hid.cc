/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_hid.h"

#include <utility>

#include "xenia/hid/remote/remote_input_driver.h"

namespace xe {
namespace hid {
namespace remote {

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order,
                                    std::shared_ptr<RemotePads> pads) {
  if (!pads) {
    return nullptr;
  }
  return std::make_unique<RemoteInputDriver>(window, window_z_order,
                                             std::move(pads));
}

}  // namespace remote
}  // namespace hid
}  // namespace xe
