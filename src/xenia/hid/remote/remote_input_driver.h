/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_REMOTE_REMOTE_INPUT_DRIVER_H_
#define XENIA_HID_REMOTE_REMOTE_INPUT_DRIVER_H_

#include <memory>

#include "xenia/hid/input_driver.h"
#include "xenia/hid/remote/remote_pads.h"

namespace xe {
namespace hid {
namespace remote {

// The guest's view of the pads a tool drives over the automation channel.
class RemoteInputDriver final : public InputDriver {
 public:
  RemoteInputDriver(xe::ui::Window* window, size_t window_z_order,
                    std::shared_ptr<RemotePads> pads);

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    return pads_->GetCapabilities(user_index, flags, out_caps);
  }
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    return pads_->GetState(user_index, out_state);
  }
  X_RESULT SetState(uint32_t user_index,
                    X_INPUT_VIBRATION* vibration) override {
    return pads_->SetState(user_index, vibration);
  }
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override {
    return pads_->GetKeystroke(user_index, flags, out_keystroke);
  }
  InputType GetInputType() const override { return InputType::Controller; }

 private:
  std::shared_ptr<RemotePads> pads_;
};

}  // namespace remote
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_REMOTE_REMOTE_INPUT_DRIVER_H_
