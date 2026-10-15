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

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/remote/remote_hid.h"
#include "xenia/hid/remote/remote_pad.h"

namespace xe {
namespace hid {
namespace remote {

#if XE_PLATFORM_WIN32
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

class RemoteInputDriver final : public InputDriver {
 public:
  RemoteInputDriver(xe::ui::Window* window, size_t window_z_order,
                    uint16_t port, FrameSource frame_source);
  ~RemoteInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  InputType GetInputType() const override { return InputType::Controller; }

 private:
  struct User {
    bool connected = false;
    PadQueue queue;
    PadState last_state;
    uint32_t packet_number = 0;
    uint32_t seen_id = 0;
    uint32_t keystroke_bits = 0;
  };

  // Both require mutex_. guest_read marks the visible queued state as seen.
  const PadState& CurrentState(uint32_t user_index, uint64_t vblank,
                               bool guest_read);
  void ResetUsers();

  void ServerLoop();
  void AcceptClient();
  void ReadClient();
  void HandleLine(std::string_view line);
  void SendLine(std::string_view line);
  void DropClient();

  uint16_t port_;
  FrameSource frame_source_;

  std::mutex mutex_;
  std::array<User, kUserCount> users_;
  // Events from guest threads (seen) and the server thread, sent by the
  // server thread.
  std::vector<PadEvent> pending_events_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  // Only touched by the server thread after Setup.
  SocketHandle listen_socket_;
  SocketHandle client_socket_;
  std::string read_buffer_;
  bool watch_frames_ = false;
  uint64_t last_vblank_ = 0;
  bool winsock_started_ = false;
};

}  // namespace remote
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_REMOTE_REMOTE_INPUT_DRIVER_H_
