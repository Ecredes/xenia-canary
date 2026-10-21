/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_REMOTE_REMOTE_HID_H_
#define XENIA_HID_REMOTE_REMOTE_HID_H_

#include <memory>

#include "xenia/hid/input_system.h"
#include "xenia/hid/remote/remote_pads.h"

namespace xe {
namespace hid {
namespace remote {

// Input driver for the pads an automation channel client drives. Null
// without pads (no channel).
std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order,
                                    std::shared_ptr<RemotePads> pads);

}  // namespace remote
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_REMOTE_REMOTE_HID_H_
