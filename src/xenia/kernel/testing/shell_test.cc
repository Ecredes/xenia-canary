/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/dispatch/shell.h"

#include <atomic>
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

namespace {

// Waits until thread is waiting (or held) in the core.
void AwaitWaiting(Shell& shell, ThreadId thread) {
  while (!shell.Read([thread](Dispatcher& core) {
    std::optional<ThreadInfo> info = core.QueryThread(thread);
    return info && info->waiting;
  })) {
    std::this_thread::yield();
  }
}

}  // namespace

TEST_CASE("A suspended shell waiter is woken only after its resume",
          "[dispatch][shell]") {
  Shell shell;
  ObjectId event =
      shell.Read([](Dispatcher& core) { return core.NewEvent(false, false); });
  std::atomic<Waiter*> target{nullptr};
  std::atomic<uint32_t> status{0xFFFFFFFF};
  std::thread waiter([&] {
    target = &shell.CurrentWaiter();
    WaitRequest request;
    request.objects = {event};
    status = shell.Wait(request);
  });
  while (!target) {
    std::this_thread::yield();
  }
  AwaitWaiting(shell, target.load()->id);
  REQUIRE(shell.Suspend(*target) == 0);
  shell.Run([event](Dispatcher& core) { return core.SetEvent(event); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(status == 0xFFFFFFFF);
  REQUIRE(shell.Read([event](Dispatcher& core) {
    return core.Query(event).value_or(ObjectInfo{}).signal_state;
  }) == 1);
  REQUIRE(shell.Resume(*target) == 1);
  waiter.join();
  REQUIRE(status == kStatusWait0);
}

TEST_CASE("A thread that suspends itself returns once resumed",
          "[dispatch][shell]") {
  Shell shell;
  std::atomic<Waiter*> target{nullptr};
  std::atomic<bool> returned{false};
  std::thread self([&] {
    Waiter& mine = shell.CurrentWaiter();
    target = &mine;
    shell.Suspend(mine);
    returned = true;
  });
  while (!target) {
    std::this_thread::yield();
  }
  AwaitWaiting(shell, target.load()->id);
  REQUIRE(!returned);
  REQUIRE(shell.Suspend(*target) == 1);
  REQUIRE(shell.Resume(*target) == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(!returned);
  REQUIRE(shell.Resume(*target) == 1);
  self.join();
  REQUIRE(returned);
}

TEST_CASE("A timer routine reaches its waiter and ends its alertable wait",
          "[dispatch][shell]") {
  Shell shell;
  auto [timer, event] = shell.Read([](Dispatcher& core) {
    return std::make_pair(core.NewTimer(true), core.NewEvent(false, false));
  });
  std::atomic<Waiter*> target{nullptr};
  std::atomic<uint32_t> status{0xFFFFFFFF};
  // Taken on the thread: a host thread's waiter goes with it.
  std::vector<TimerApc> apcs, again;
  ThreadId id = kNoThread;
  std::thread waiter([&, timer = timer, event = event] {
    Waiter& mine = shell.CurrentWaiter();
    shell.Run([&](Dispatcher& core) {
      return core.SetTimer(timer, Shell::GuestNow(), 0,
                           TimerRoutine{mine.id, 0x82000000, 7});
    });
    target = &mine;
    WaitRequest request;
    request.objects = {event};
    request.alertable = true;
    id = mine.id;
    status = shell.Wait(request);
    apcs = shell.TakeTimerApcs(mine);
    again = shell.TakeTimerApcs(mine);
  });
  while (!target) {
    std::this_thread::yield();
  }
  AwaitWaiting(shell, target.load()->id);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  shell.Run([](Dispatcher& core) { return core.Tick(Shell::GuestNow()); });
  waiter.join();
  REQUIRE(status == kStatusUserApc);
  REQUIRE(apcs.size() == 1);
  REQUIRE(apcs[0].thread == id);
  REQUIRE(apcs[0].routine == 0x82000000);
  REQUIRE(apcs[0].argument == 7);
  REQUIRE(again.empty());
}

TEST_CASE("Tick on a host thread's routine timer signals and queues nothing",
          "[dispatch][shell]") {
  Shell shell;
  ObjectId timer =
      shell.Read([](Dispatcher& core) { return core.NewTimer(true); });
  // No routine: what XTimer arms when no XThread is current.
  shell.Run([&](Dispatcher& core) {
    return core.SetTimer(timer, Shell::GuestNow(), 0, std::nullopt);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  shell.Run([](Dispatcher& core) { return core.Tick(Shell::GuestNow()); });
  int32_t state = shell.Read([&](Dispatcher& core) {
    return core.Query(timer).value_or(ObjectInfo{}).signal_state;
  });
  REQUIRE(state == 1);
  REQUIRE(shell.TakeTimerApcs(shell.CurrentWaiter()).empty());
}

}  // namespace test
}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
