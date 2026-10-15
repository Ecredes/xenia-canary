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
#include <charconv>
#include <cstdlib>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/ui/virtual_key.h"

namespace xe::hid::remote {

namespace {

constexpr size_t kMaxTokens = 12;

// Splits on spaces; returns false if there are too many tokens.
bool Tokenize(std::string_view line,
              std::array<std::string_view, kMaxTokens>* tokens, size_t* count) {
  *count = 0;
  size_t pos = 0;
  while (pos < line.size()) {
    if (line[pos] == ' ') {
      ++pos;
      continue;
    }
    size_t end = line.find(' ', pos);
    if (end == std::string_view::npos) {
      end = line.size();
    }
    if (*count == kMaxTokens) {
      return false;
    }
    (*tokens)[(*count)++] = line.substr(pos, end - pos);
    pos = end;
  }
  return true;
}

template <typename T>
bool ParseNumber(std::string_view token, int base, int64_t min, int64_t max,
                 T* out) {
  int64_t value = 0;
  auto [ptr, ec] =
      std::from_chars(token.data(), token.data() + token.size(), value, base);
  if (ec != std::errc() || ptr != token.data() + token.size() || value < min ||
      value > max) {
    return false;
  }
  *out = static_cast<T>(value);
  return true;
}

// Parses the 7 <pad> fields starting at tokens[first].
bool ParsePad(const std::array<std::string_view, kMaxTokens>& tokens,
              size_t first, PadState* state) {
  return ParseNumber(tokens[first], 16, 0, 0xFFFF, &state->buttons) &&
         ParseNumber(tokens[first + 1], 10, 0, 255, &state->left_trigger) &&
         ParseNumber(tokens[first + 2], 10, 0, 255, &state->right_trigger) &&
         ParseNumber(tokens[first + 3], 10, -32768, 32767, &state->thumb_lx) &&
         ParseNumber(tokens[first + 4], 10, -32768, 32767, &state->thumb_ly) &&
         ParseNumber(tokens[first + 5], 10, -32768, 32767, &state->thumb_rx) &&
         ParseNumber(tokens[first + 6], 10, -32768, 32767, &state->thumb_ry);
}

bool ParseUser(std::string_view token, uint32_t* user) {
  return ParseNumber(token, 10, 0, kUserCount - 1, user);
}

}  // namespace

std::optional<Command> ParseCommand(std::string_view line, std::string* error) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  std::array<std::string_view, kMaxTokens> tokens;
  size_t count = 0;
  if (line.size() > kMaxLineLength || !Tokenize(line, &tokens, &count)) {
    *error = "line too long";
    return std::nullopt;
  }
  if (count == 0) {
    *error = "empty command";
    return std::nullopt;
  }
  Command command;
  const std::string_view name = tokens[0];
  bool ok = false;
  if (name == "hello" && count == 1) {
    command.type = Command::Type::kHello;
    ok = true;
  } else if (name == "ping" && count == 1) {
    command.type = Command::Type::kPing;
    ok = true;
  } else if (name == "connect" && count == 3) {
    command.type = Command::Type::kConnect;
    ok = ParseUser(tokens[1], &command.user) &&
         ParseNumber(tokens[2], 10, 0, 1, &command.value);
  } else if (name == "set" && count == 9) {
    command.type = Command::Type::kSet;
    ok = ParseUser(tokens[1], &command.user) &&
         ParsePad(tokens, 2, &command.state);
  } else if (name == "queue" && count == 11) {
    command.type = Command::Type::kQueue;
    ok = ParseUser(tokens[1], &command.user) &&
         ParseNumber(tokens[2], 10, 1, INT32_MAX, &command.id) &&
         ParseNumber(tokens[3], 10, 1, kMaxFrames, &command.frames) &&
         ParsePad(tokens, 4, &command.state);
  } else if (name == "clear" && count == 2) {
    command.type = Command::Type::kClear;
    ok = ParseUser(tokens[1], &command.user);
  } else if (name == "watch" && count == 2) {
    command.type = Command::Type::kWatch;
    ok = ParseNumber(tokens[1], 10, 0, 1, &command.value);
  } else {
    *error = fmt::format("unknown command or wrong argument count: {}",
                         std::string(name));
    return std::nullopt;
  }
  if (!ok) {
    *error = fmt::format("bad arguments for {}", std::string(name));
    return std::nullopt;
  }
  return command;
}

std::string FormatEvent(const PadEvent& event, uint64_t ns) {
  const char* kind = "begin";
  switch (event.kind) {
    case PadEvent::Kind::kBegin:
      kind = "begin";
      break;
    case PadEvent::Kind::kSeen:
      kind = "seen";
      break;
    case PadEvent::Kind::kEnd:
      kind = "end";
      break;
  }
  return fmt::format("{} {} {} {} {}\n", kind, event.user, event.id,
                     event.vblank, ns);
}

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
