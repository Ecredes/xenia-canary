/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_pads.h"

#include <utility>

namespace xe::hid::remote {

RemotePads::RemotePads(FrameSource frame_source)
    : frame_source_(std::move(frame_source)) {}

std::string RemotePads::Apply(const Command& command) {
  const uint64_t vblank = frame_source_().vblank;
  std::lock_guard<std::mutex> lock(mutex_);
  User& user = users_[command.user];
  switch (command.type) {
    case Command::Type::kConnect:
      user.connected = command.value != 0;
      if (!user.connected) {
        user.queue.Clear();
        user.queue.set_base(PadState());
      }
      break;
    case Command::Type::kSet:
      user.queue.set_base(command.state);
      break;
    case Command::Type::kQueue:
      if (!user.queue.Enqueue(command.id, command.frames, command.state,
                              vblank)) {
        return "queue full";
      }
      break;
    case Command::Type::kClear:
      user.queue.Clear();
      break;
  }
  return {};
}

std::vector<PadEvent> RemotePads::Poll() {
  const uint64_t vblank = frame_source_().vblank;
  std::vector<PadEvent> events;
  std::lock_guard<std::mutex> lock(mutex_);
  for (uint32_t user_index = 0; user_index < kUserCount; ++user_index) {
    if (users_[user_index].connected) {
      CurrentState(user_index, vblank, false);
    }
  }
  events.swap(pending_events_);
  return events;
}

void RemotePads::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  ResetUsers();
  pending_events_.clear();
}

const PadState& RemotePads::CurrentState(uint32_t user_index, uint64_t vblank,
                                         bool guest_read) {
  User& user = users_[user_index];
  const PadState& state =
      user.queue.Advance(vblank, user_index, &pending_events_);
  const uint32_t active_id = user.queue.active_id();
  if (guest_read && active_id != 0 && active_id != user.seen_id) {
    user.seen_id = active_id;
    pending_events_.push_back(
        {PadEvent::Kind::kSeen, user_index, active_id, vblank});
  }
  if (state != user.last_state) {
    user.last_state = state;
    ++user.packet_number;
  }
  return state;
}

void RemotePads::ResetUsers() {
  for (User& user : users_) {
    user.connected = false;
    user.queue.Clear();
    user.queue.set_base(PadState());
  }
}

X_RESULT RemotePads::GetCapabilities(uint32_t user_index, uint32_t flags,
                                     X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= kUserCount || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!users_[user_index].connected) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(-1);
  out_caps->gamepad.thumb_ly = int16_t(-1);
  out_caps->gamepad.thumb_rx = int16_t(-1);
  out_caps->gamepad.thumb_ry = int16_t(-1);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT RemotePads::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index >= kUserCount || !out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const uint64_t vblank = frame_source_().vblank;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!users_[user_index].connected) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const PadState& state = CurrentState(user_index, vblank, true);
  out_state->packet_number = users_[user_index].packet_number;
  out_state->gamepad.buttons = state.buttons;
  out_state->gamepad.left_trigger = state.left_trigger;
  out_state->gamepad.right_trigger = state.right_trigger;
  out_state->gamepad.thumb_lx = state.thumb_lx;
  out_state->gamepad.thumb_ly = state.thumb_ly;
  out_state->gamepad.thumb_rx = state.thumb_rx;
  out_state->gamepad.thumb_ry = state.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT RemotePads::SetState(uint32_t user_index,
                              X_INPUT_VIBRATION* vibration) {
  if (user_index >= kUserCount) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return users_[user_index].connected ? X_ERROR_SUCCESS
                                      : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT RemotePads::GetKeystroke(uint32_t users, uint32_t flags,
                                  X_INPUT_KEYSTROKE* out_keystroke) {
  const bool any_user = users == XUserIndexAny;
  if ((users >= kUserCount && !any_user) || !out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const uint64_t vblank = frame_source_().vblank;
  std::lock_guard<std::mutex> lock(mutex_);
  bool any_connected = false;
  const uint32_t first = any_user ? 0 : users;
  const uint32_t last = any_user ? kUserCount : users + 1;
  for (uint32_t user_index = first; user_index < last; ++user_index) {
    User& user = users_[user_index];
    if (!user.connected) {
      continue;
    }
    any_connected = true;
    const uint32_t bits = KeystrokeBits(CurrentState(user_index, vblank, true));
    auto key = NextKeystroke(bits, &user.keystroke_bits);
    if (!key) {
      continue;
    }
    out_keystroke->virtual_key = key->virtual_key;
    out_keystroke->unicode = 0;
    out_keystroke->flags =
        key->down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
    out_keystroke->user_index = uint8_t(user_index);
    out_keystroke->hid_code = 0;
    return X_ERROR_SUCCESS;
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace xe::hid::remote
