/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_REMOTE_REMOTE_PADS_H_
#define XENIA_HID_REMOTE_REMOTE_PADS_H_

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/hid/input.h"
#include "xenia/hid/remote/remote_pad.h"
#include "xenia/xbox.h"

namespace xe::hid::remote {

struct FrameCounters {
  uint64_t vblank = 0;
  uint64_t swap = 0;
};
using FrameSource = std::function<FrameCounters()>;

// The pads a tool drives: commands from the tool's thread, reads from guest
// threads through RemoteInputDriver. Thread-safe.
class RemotePads {
 public:
  explicit RemotePads(FrameSource frame_source);

  // Applies a checked command. Returns an error, empty on success.
  std::string Apply(const Command& command);
  // Moves every connected pad to the current vblank, so queued states begin
  // and end even while the guest doesn't poll, and returns the begin, seen and
  // end events since the last call, in order.
  std::vector<PadEvent> Poll();
  // The tool is gone: every pad disconnects and its queue is dropped.
  void Reset();

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

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

  FrameSource frame_source_;
  std::mutex mutex_;
  std::array<User, kUserCount> users_;
  // Events from guest threads (seen) and Poll, taken by Poll.
  std::vector<PadEvent> pending_events_;
};

}  // namespace xe::hid::remote

#endif  // XENIA_HID_REMOTE_REMOTE_PADS_H_
