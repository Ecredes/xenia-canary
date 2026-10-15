/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_pad.h"

#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/ui/virtual_key.h"

namespace xe::hid::remote::test {

namespace {
PadState Buttons(uint16_t buttons) {
  PadState state;
  state.buttons = buttons;
  return state;
}
}  // namespace

TEST_CASE("Parse set command", "[remote_input]") {
  std::string error;
  auto command = ParseCommand("set 1 1000 255 0 -32768 32767 5 -5", &error);
  REQUIRE(command.has_value());
  REQUIRE(command->type == Command::Type::kSet);
  REQUIRE(command->user == 1);
  REQUIRE(command->state.buttons == 0x1000);
  REQUIRE(command->state.left_trigger == 255);
  REQUIRE(command->state.right_trigger == 0);
  REQUIRE(command->state.thumb_lx == -32768);
  REQUIRE(command->state.thumb_ly == 32767);
  REQUIRE(command->state.thumb_rx == 5);
  REQUIRE(command->state.thumb_ry == -5);
}

TEST_CASE("Parse queue command", "[remote_input]") {
  std::string error;
  auto command = ParseCommand("queue 0 42 3 f000 0 0 0 0 0 0", &error);
  REQUIRE(command.has_value());
  REQUIRE(command->type == Command::Type::kQueue);
  REQUIRE(command->id == 42);
  REQUIRE(command->frames == 3);
  REQUIRE(command->state.buttons == 0xF000);
}

TEST_CASE("Parse simple commands", "[remote_input]") {
  std::string error;
  REQUIRE(ParseCommand("hello", &error)->type == Command::Type::kHello);
  REQUIRE(ParseCommand("ping", &error)->type == Command::Type::kPing);
  REQUIRE(ParseCommand("watch 1", &error)->value == 1);
  REQUIRE(ParseCommand("connect 3 0", &error)->user == 3);
  REQUIRE(ParseCommand("clear 2", &error)->type == Command::Type::kClear);
  // Trailing carriage returns from Windows clients are accepted.
  REQUIRE(ParseCommand("ping\r", &error).has_value());
}

TEST_CASE("Reject malformed commands", "[remote_input]") {
  std::string error;
  for (const char* line :
       {"", "bogus", "set 4 0 0 0 0 0 0 0", "set 0 0 256 0 0 0 0 0",
        "set 0 0 0 0 32768 0 0 0", "set 0 10000 0 0 0 0 0 0",
        "set 0 0 0 0 0 0 0", "queue 0 1 0 0 0 0 0 0 0 0",
        "queue 0 0 1 0 0 0 0 0 0 0", "queue 0 1 3601 0 0 0 0 0 0 0", "watch 2",
        "connect 0", "set 0 zz 0 0 0 0 0 0", "ping extra"}) {
    error.clear();
    INFO(line);
    REQUIRE_FALSE(ParseCommand(line, &error).has_value());
    REQUIRE_FALSE(error.empty());
  }
}

TEST_CASE("Base state shows when the queue is empty", "[remote_input]") {
  PadQueue queue;
  queue.set_base(Buttons(0x1000));
  std::vector<PadEvent> events;
  REQUIRE(queue.Advance(10, 0, &events).buttons == 0x1000);
  REQUIRE(queue.active_id() == 0);
  REQUIRE(events.empty());
}

TEST_CASE("Queued state holds for exactly its frames", "[remote_input]") {
  PadQueue queue;
  std::vector<PadEvent> events;
  // Queued during vblank 100: starts on the next vblank, 101..102.
  REQUIRE(queue.Enqueue(7, 2, Buttons(0x1000), 100));
  REQUIRE(queue.Advance(100, 0, &events).buttons == 0);
  REQUIRE(events.empty());
  REQUIRE(queue.Advance(101, 0, &events).buttons == 0x1000);
  REQUIRE(queue.active_id() == 7);
  REQUIRE(queue.Advance(102, 0, &events).buttons == 0x1000);
  REQUIRE(queue.Advance(103, 0, &events).buttons == 0);
  REQUIRE(queue.active_id() == 0);
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].kind == PadEvent::Kind::kBegin);
  REQUIRE(events[0].id == 7);
  REQUIRE(events[0].vblank == 101);
  REQUIRE(events[1].kind == PadEvent::Kind::kEnd);
  REQUIRE(events[1].vblank == 103);
}

TEST_CASE("Queued states run back to back", "[remote_input]") {
  PadQueue queue;
  std::vector<PadEvent> events;
  REQUIRE(queue.Enqueue(1, 1, Buttons(0x1), 50));
  REQUIRE(queue.Enqueue(2, 2, Buttons(0x2), 50));
  REQUIRE(queue.Advance(51, 0, &events).buttons == 0x1);
  REQUIRE(queue.Advance(52, 0, &events).buttons == 0x2);
  REQUIRE(queue.Advance(53, 0, &events).buttons == 0x2);
  REQUIRE(queue.Advance(54, 0, &events).buttons == 0);
}

TEST_CASE("A late observer still gets every event in order", "[remote_input]") {
  PadQueue queue;
  std::vector<PadEvent> events;
  REQUIRE(queue.Enqueue(1, 1, Buttons(0x1), 10));
  REQUIRE(queue.Enqueue(2, 1, Buttons(0x2), 10));
  // Nothing observed until well after both ended.
  REQUIRE(queue.Advance(20, 0, &events).buttons == 0);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].kind == PadEvent::Kind::kBegin);
  REQUIRE(events[0].vblank == 11);
  REQUIRE(events[1].kind == PadEvent::Kind::kEnd);
  REQUIRE(events[1].vblank == 12);
  REQUIRE(events[2].id == 2);
  REQUIRE(events[2].vblank == 12);
  REQUIRE(events[3].vblank == 13);
}

TEST_CASE("Queue is bounded", "[remote_input]") {
  PadQueue queue;
  for (uint32_t i = 0; i < kMaxQueue; ++i) {
    REQUIRE(queue.Enqueue(i + 1, 1, Buttons(0), 0));
  }
  REQUIRE_FALSE(queue.Enqueue(9999, 1, Buttons(0), 0));
}

TEST_CASE("Clear drops queued states", "[remote_input]") {
  PadQueue queue;
  std::vector<PadEvent> events;
  REQUIRE(queue.Enqueue(1, 10, Buttons(0x1), 0));
  REQUIRE(queue.Advance(2, 0, &events).buttons == 0x1);
  queue.Clear();
  REQUIRE(queue.Advance(3, 0, &events).buttons == 0);
  // A queue after a clear starts on the next vblank.
  REQUIRE(queue.Enqueue(2, 1, Buttons(0x2), 3));
  REQUIRE(queue.Advance(4, 0, &events).buttons == 0x2);
}

TEST_CASE("Format events", "[remote_input]") {
  PadEvent event{PadEvent::Kind::kBegin, 2, 7, 101};
  REQUIRE(FormatEvent(event, 123456789) == "begin 2 7 101 123456789\n");
  event.kind = PadEvent::Kind::kSeen;
  REQUIRE(FormatEvent(event, 5) == "seen 2 7 101 5\n");
}

TEST_CASE("Keystrokes follow button changes", "[remote_input]") {
  uint32_t last = 0;
  uint32_t current = KeystrokeBits(Buttons(0x1000));  // A
  auto key = NextKeystroke(current, &last);
  REQUIRE(key.has_value());
  REQUIRE(key->virtual_key == uint16_t(ui::VirtualKey::kXInputPadA));
  REQUIRE(key->down);
  REQUIRE_FALSE(NextKeystroke(current, &last).has_value());

  // Releases come before presses.
  current = KeystrokeBits(Buttons(0x2000));  // B
  key = NextKeystroke(current, &last);
  REQUIRE(key->virtual_key == uint16_t(ui::VirtualKey::kXInputPadA));
  REQUIRE_FALSE(key->down);
  key = NextKeystroke(current, &last);
  REQUIRE(key->virtual_key == uint16_t(ui::VirtualKey::kXInputPadB));
  REQUIRE(key->down);
}

TEST_CASE("Keystrokes from triggers and sticks", "[remote_input]") {
  PadState state;
  state.right_trigger = 200;
  state.thumb_lx = 30000;
  state.thumb_ly = 10000;  // Diagonal resolves to the dominant axis.
  uint32_t last = 0;
  uint32_t current = KeystrokeBits(state);
  std::vector<uint16_t> keys;
  for (int i = 0; i < 8; ++i) {
    auto key = NextKeystroke(current, &last);
    if (!key) {
      break;
    }
    keys.push_back(key->virtual_key);
  }
  REQUIRE(keys == std::vector<uint16_t>{
                      uint16_t(ui::VirtualKey::kXInputPadRTrigger),
                      uint16_t(ui::VirtualKey::kXInputPadLThumbRight)});
  // Inside the deadzone nothing is pressed.
  state = PadState();
  state.thumb_ly = 5000;
  REQUIRE(KeystrokeBits(state) == 0);
}

}  // namespace xe::hid::remote::test
