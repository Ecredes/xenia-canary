/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_REMOTE_REMOTE_PAD_H_
#define XENIA_HID_REMOTE_REMOTE_PAD_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Protocol and per-user state for the remote input driver. Kept free of
// sockets and threads so it can be tested on its own.
//
// The protocol is line based ASCII, one command per '\n' terminated line:
//   hello                          -> hello xenia-remote-input <ver> <users>
//   ping                           -> pong <vblank> <swap> <ns>
//   connect <user> <0|1>           -> ok
//   set <user> <pad>               -> ok    (held until changed)
//   queue <user> <id> <frames> <pad> -> ok  (held for <frames> vblanks)
//   clear <user>                   -> ok    (drops queued states)
//   watch <0|1>                    -> ok    (frame lines on every vblank)
// where <pad> is: <buttons hex> <lt> <rt> <lx> <ly> <rx> <ry>, using the
// X_INPUT_GAMEPAD layout and ranges.
//
// Notifications, each tagged with the vblank count and steady clock ns:
//   begin <user> <id> <vblank> <ns>   a queued state became visible
//   seen <user> <id> <vblank> <ns>    the guest first read it
//   end <user> <id> <vblank> <ns>     it stopped being visible
//   frame <vblank> <swap> <ns>        (watch 1 only)
// Errors are reported as: error <message>

namespace xe::hid::remote {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kUserCount = 4;
constexpr uint32_t kMaxQueue = 256;
constexpr uint32_t kMaxFrames = 3600;
constexpr size_t kMaxLineLength = 256;

struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;

  bool operator==(const PadState&) const = default;
};

struct Command {
  enum class Type { kHello, kPing, kConnect, kSet, kQueue, kClear, kWatch };
  Type type = Type::kPing;
  uint32_t user = 0;
  uint32_t id = 0;
  uint32_t frames = 0;
  uint32_t value = 0;
  PadState state;
};

// Returns the parsed command, or nullopt with a message in *error.
std::optional<Command> ParseCommand(std::string_view line, std::string* error);

struct PadEvent {
  enum class Kind { kBegin, kSeen, kEnd };
  Kind kind = Kind::kBegin;
  uint32_t user = 0;
  uint32_t id = 0;
  uint64_t vblank = 0;
};

std::string FormatEvent(const PadEvent& event, uint64_t ns);

// Keystroke synthesis for XInputGetKeystroke: one bit per button, trigger and
// stick direction (sticks report only their dominant axis, no diagonals).
uint32_t KeystrokeBits(const PadState& state);

struct Keystroke {
  uint16_t virtual_key;
  bool down;
};

// Returns the next change from *last toward current, releases first, and
// applies it to *last. Nullopt when they match.
std::optional<Keystroke> NextKeystroke(uint32_t current, uint32_t* last);

// One user's pad: a base state plus a queue of states that each last a fixed
// number of vblanks. Queued states start on the vblank after they are queued
// (so each gets whole vblank periods) or right after the previous one ends.
class PadQueue {
 public:
  void set_base(const PadState& state) { base_ = state; }
  const PadState& base() const { return base_; }

  // Returns false if the queue is full.
  bool Enqueue(uint32_t id, uint32_t frames, const PadState& state,
               uint64_t now_vblank);
  void Clear();

  // Moves to vblank `now`, appending begin and end events for every state
  // that started or finished since the last call, and returns the visible
  // state.
  const PadState& Advance(uint64_t now_vblank, uint32_t user,
                          std::vector<PadEvent>* events);

  // Id of the queued state visible after the last Advance, 0 for the base.
  uint32_t active_id() const { return active_id_; }

 private:
  struct Item {
    uint32_t id;
    uint64_t start;
    uint64_t end;
    PadState state;
    bool begun;
  };
  std::deque<Item> items_;
  PadState base_;
  uint32_t active_id_ = 0;
};

}  // namespace xe::hid::remote

#endif  // XENIA_HID_REMOTE_REMOTE_PAD_H_
