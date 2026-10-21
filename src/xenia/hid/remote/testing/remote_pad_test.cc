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
