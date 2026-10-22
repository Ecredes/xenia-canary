/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/dispatch/shell.h"

#include <thread>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace dispatch {
namespace test {

TEST_CASE("A shell wait is woken by a signal on another thread",
          "[dispatch][shell]") {
  Shell shell;
  ObjectId event =
      shell.Read([](Dispatcher& core) { return core.NewEvent(false, false); });
  WaitRequest request;
  request.objects = {event};
  uint32_t status = 0xFFFFFFFF;
  std::thread waiter([&] { status = shell.Wait(request); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  shell.Run([event](Dispatcher& core) { return core.SetEvent(event); });
  waiter.join();
  REQUIRE(status == kStatusWait0);
}

TEST_CASE("A shell wait times out on guest time", "[dispatch][shell]") {
  Shell shell;
  ObjectId event =
      shell.Read([](Dispatcher& core) { return core.NewEvent(true, false); });
  WaitRequest request;
  request.objects = {event};
  // 20 ms, relative.
  uint64_t timeout = uint64_t(-200000);
  request.deadline = Shell::DeadlineFromTimeout(&timeout);
  REQUIRE(shell.Wait(request) == kStatusTimeout);
  REQUIRE(Shell::GuestNow() >= request.deadline.value_or(UINT64_MAX));
}

TEST_CASE("Shell waits ping-pong between two threads", "[dispatch][shell]") {
  Shell shell;
  auto [ping, pong] = shell.Read([](Dispatcher& core) {
    return std::make_pair(core.NewEvent(false, false),
                          core.NewEvent(false, false));
  });
  constexpr int kRounds = 2000;
  std::thread other([&, ping = ping, pong = pong] {
    WaitRequest request;
    request.objects = {ping};
    for (int i = 0; i < kRounds; ++i) {
      shell.Wait(request);
      shell.Run([pong](Dispatcher& core) { return core.SetEvent(pong); });
    }
  });
  WaitRequest request;
  request.objects = {pong};
  int rounds = 0;
  for (; rounds < kRounds; ++rounds) {
    shell.Run([ping](Dispatcher& core) { return core.SetEvent(ping); });
    if (shell.Wait(request) != kStatusWait0) {
      break;
    }
  }
  other.join();
  REQUIRE(rounds == kRounds);
}

}  // namespace test
}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
