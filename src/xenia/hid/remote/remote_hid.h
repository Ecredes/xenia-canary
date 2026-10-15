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

#include <cstdint>
#include <functional>
#include <memory>

#include "xenia/hid/input_system.h"

namespace xe {
namespace hid {
namespace remote {

struct FrameCounters {
  uint64_t vblank = 0;
  uint64_t swap = 0;
};
using FrameSource = std::function<FrameCounters()>;

// Input driver fed over a local TCP socket (127.0.0.1 only), for automated
// testing tools. See remote_pad.h for the protocol. Returns null when
// remote_input_port is 0.
std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order,
                                    FrameSource frame_source);

}  // namespace remote
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_REMOTE_REMOTE_HID_H_
