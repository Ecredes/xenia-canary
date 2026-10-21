/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_pad.h"

#include <algorithm>
#include <array>
#include <cstdlib>

#include "xenia/ui/virtual_key.h"

namespace xe::hid::remote {

namespace {

using ui::VirtualKey;

// Bits 0-15 follow X_INPUT_GAMEPAD buttons; the rest are synthesized.
constexpr std::array<VirtualKey, 26> kKeystrokeKeys = {
    VirtualKey::kXInputPadDpadUp,
    VirtualKey::kXInputPadDpadDown,
    VirtualKey::kXInputPadDpadLeft,
    VirtualKey::kXInputPadDpadRight,
    VirtualKey::kXInputPadStart,
    VirtualKey::kXInputPadBack,
    VirtualKey::kXInputPadLThumbPress,
    VirtualKey::kXInputPadRThumbPress,
    VirtualKey::kXInputPadLShoulder,
    VirtualKey::kXInputPadRShoulder,
    VirtualKey::kXInputPadGuide,
    VirtualKey::kNone,
    VirtualKey::kXInputPadA,
    VirtualKey::kXInputPadB,
    VirtualKey::kXInputPadX,
    VirtualKey::kXInputPadY,
    // 16: triggers
    VirtualKey::kXInputPadLTrigger,
    VirtualKey::kXInputPadRTrigger,
    // 18: left stick up, down, right, left
    VirtualKey::kXInputPadLThumbUp,
    VirtualKey::kXInputPadLThumbDown,
    VirtualKey::kXInputPadLThumbRight,
    VirtualKey::kXInputPadLThumbLeft,
    // 22: right stick up, down, right, left
    VirtualKey::kXInputPadRThumbUp,
    VirtualKey::kXInputPadRThumbDown,
    VirtualKey::kXInputPadRThumbRight,
    VirtualKey::kXInputPadRThumbLeft,
};

constexpr uint8_t kTriggerThreshold = 30;  // X_INPUT_GAMEPAD_TRIGGER_THRESHOLD
constexpr int32_t kStickDeadzone = 7849;   // Left stick deadzone.

// Returns the bit offset (0 up, 1 down, 2 right, 3 left) or -1.
int StickDirection(int16_t x, int16_t y) {
  const int32_t ax = std::abs(int32_t(x));
  const int32_t ay = std::abs(int32_t(y));
  if (std::max(ax, ay) <= kStickDeadzone) {
    return -1;
  }
  if (ay >= ax) {
    return y > 0 ? 0 : 1;
  }
  return x > 0 ? 2 : 3;
}

}  // namespace

uint32_t KeystrokeBits(const PadState& state) {
  uint32_t bits = state.buttons;
  if (state.left_trigger > kTriggerThreshold) {
    bits |= 1u << 16;
  }
  if (state.right_trigger > kTriggerThreshold) {
    bits |= 1u << 17;
  }
  int left = StickDirection(state.thumb_lx, state.thumb_ly);
  if (left >= 0) {
    bits |= 1u << (18 + left);
  }
  int right = StickDirection(state.thumb_rx, state.thumb_ry);
  if (right >= 0) {
    bits |= 1u << (22 + right);
  }
  return bits;
}

std::optional<Keystroke> NextKeystroke(uint32_t current, uint32_t* last) {
  for (bool release_pass : {true, false}) {
    for (uint32_t i = 0; i < kKeystrokeKeys.size(); ++i) {
      const uint32_t bit = 1u << i;
      if (!((current ^ *last) & bit) ||
          kKeystrokeKeys[i] == VirtualKey::kNone) {
        continue;
      }
      const bool down = (current & bit) != 0;
      if (down == release_pass) {
        continue;
      }
      *last ^= bit;
      return Keystroke{uint16_t(kKeystrokeKeys[i]), down};
    }
  }
  // Unmapped bits (bit 11) never produce keystrokes; settle them silently.
  *last = current;
  return std::nullopt;
}

bool PadQueue::Enqueue(uint32_t id, uint32_t frames, const PadState& state,
                       uint64_t now_vblank) {
  if (items_.size() >= kMaxQueue) {
    return false;
  }
  uint64_t start = now_vblank + 1;
  if (!items_.empty()) {
    start = std::max(start, items_.back().end);
  }
  items_.push_back({id, start, start + frames, state, false});
  return true;
}

void PadQueue::Clear() {
  items_.clear();
  active_id_ = 0;
}

const PadState& PadQueue::Advance(uint64_t now_vblank, uint32_t user,
                                  std::vector<PadEvent>* events) {
  // Bounded by the queue length: each pass retires one item.
  while (!items_.empty() && items_.front().start <= now_vblank) {
    Item& front = items_.front();
    if (!front.begun) {
      front.begun = true;
      events->push_back({PadEvent::Kind::kBegin, user, front.id, front.start});
    }
    if (front.end > now_vblank) {
      active_id_ = front.id;
      return front.state;
    }
    events->push_back({PadEvent::Kind::kEnd, user, front.id, front.end});
    items_.pop_front();
  }
  active_id_ = 0;
  return base_;
}

}  // namespace xe::hid::remote
