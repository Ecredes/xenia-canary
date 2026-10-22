/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/dispatch/dispatcher.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace dispatch {
namespace test {

namespace {

WaitRequest Wait(std::vector<ObjectId> objects, WaitType type = WaitType::kAny,
                 std::optional<uint64_t> deadline = std::nullopt,
                 bool alertable = false) {
  WaitRequest request;
  request.objects = std::move(objects);
  request.type = type;
  request.deadline = deadline;
  request.alertable = alertable;
  return request;
}

ObjectInfo Info(const Dispatcher& d, ObjectId object) {
  std::optional<ObjectInfo> info = d.Query(object);
  REQUIRE(info.has_value());
  return info.value_or(ObjectInfo{});
}

ThreadInfo Thread(const Dispatcher& d, ThreadId thread) {
  std::optional<ThreadInfo> info = d.QueryThread(thread);
  REQUIRE(info.has_value());
  return info.value_or(ThreadInfo{});
}

TimerInfo Timer(const Dispatcher& d, ObjectId timer) {
  std::optional<TimerInfo> info = d.QueryTimer(timer);
  REQUIRE(info.has_value());
  return info.value_or(TimerInfo{});
}

int32_t State(const Dispatcher& d, ObjectId object) {
  return Info(d, object).signal_state;
}

}  // namespace

TEST_CASE("A notification event wakes every waiter and stays set",
          "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  REQUIRE(d.BeginWait(a, Wait({event}), 0).blocked);
  REQUIRE(d.BeginWait(b, Wait({event}), 0).blocked);
  int32_t previous = -1;
  Effects set = d.SetEvent(event, &previous);
  REQUIRE(previous == 0);
  REQUIRE(set.wakes == std::vector<Wake>{{a, kStatusWait0}, {b, kStatusWait0}});
  REQUIRE(set.writes == std::vector<SignalWrite>{{event, 1}});
  // Still set: a new wait returns at once.
  ThreadId c = d.NewThread();
  WaitResult again = d.BeginWait(c, Wait({event}), 0);
  REQUIRE(!again.blocked);
  REQUIRE(again.status == kStatusWait0);
  REQUIRE(State(d, event) == 1);
}

TEST_CASE("A synchronization event wakes one waiter and is consumed",
          "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.BeginWait(a, Wait({event}), 0);
  d.BeginWait(b, Wait({event}), 0);
  Effects first = d.SetEvent(event);
  // First come, first served.
  REQUIRE(first.wakes == std::vector<Wake>{{a, kStatusWait0}});
  REQUIRE(State(d, event) == 0);
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{b, kStatusWait0}});
  // With nobody waiting it stays set, and the next wait takes it.
  REQUIRE(d.SetEvent(event).wakes.empty());
  REQUIRE(State(d, event) == 1);
  WaitResult taken = d.BeginWait(a, Wait({event}), 0);
  REQUIRE(!taken.blocked);
  REQUIRE(taken.effects.writes == std::vector<SignalWrite>{{event, 0}});
}

TEST_CASE("A pulse wakes the waiters there are, then resets", "[dispatch]") {
  Dispatcher d;
  ObjectId notification = d.NewEvent(true, false);
  ObjectId synchronization = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  d.BeginWait(a, Wait({notification}), 0);
  d.BeginWait(b, Wait({notification}), 0);
  d.BeginWait(c, Wait({synchronization}), 0);
  REQUIRE(d.PulseEvent(notification).wakes.size() == 2);
  REQUIRE(State(d, notification) == 0);
  REQUIRE(d.PulseEvent(synchronization).wakes ==
          std::vector<Wake>{{c, kStatusWait0}});
  // Nobody waiting: nothing is left set.
  int32_t previous = -1;
  REQUIRE(d.PulseEvent(synchronization, &previous).wakes.empty());
  REQUIRE(previous == 0);
  REQUIRE(State(d, synchronization) == 0);
}

TEST_CASE("Reset reports the previous state", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, true);
  int32_t previous = -1;
  REQUIRE(d.ResetEvent(event, &previous).writes ==
          std::vector<SignalWrite>{{event, 0}});
  REQUIRE(previous == 1);
}

TEST_CASE("A semaphore hands out its count in arrival order", "[dispatch]") {
  Dispatcher d;
  ObjectId semaphore = d.NewSemaphore(1, 3);
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  REQUIRE(!d.BeginWait(a, Wait({semaphore}), 0).blocked);
  REQUIRE(State(d, semaphore) == 0);
  d.BeginWait(b, Wait({semaphore}), 0);
  d.BeginWait(c, Wait({semaphore}), 0);
  int32_t previous = -1;
  bool exceeded = true;
  Effects release = d.ReleaseSemaphore(semaphore, 3, &previous, &exceeded);
  REQUIRE(!exceeded);
  REQUIRE(previous == 0);
  REQUIRE(release.wakes ==
          std::vector<Wake>{{b, kStatusWait0}, {c, kStatusWait0}});
  REQUIRE(State(d, semaphore) == 1);
  // Past the limit, nothing changes.
  REQUIRE(d.ReleaseSemaphore(semaphore, 3, nullptr, &exceeded).writes.empty());
  REQUIRE(exceeded);
  REQUIRE(State(d, semaphore) == 1);
  d.ReleaseSemaphore(semaphore, 0, nullptr, &exceeded);
  REQUIRE(exceeded);
}

TEST_CASE("A mutant is owned, reacquired and passed on", "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread(), b = d.NewThread();
  ObjectId mutant = d.NewMutant(kNoThread);
  REQUIRE(!d.BeginWait(a, Wait({mutant}), 0).blocked);
  // The owner reacquires; others wait.
  REQUIRE(!d.BeginWait(a, Wait({mutant}), 0).blocked);
  REQUIRE(State(d, mutant) == -1);
  REQUIRE(d.BeginWait(b, Wait({mutant}), 0).blocked);
  bool not_owned = false;
  REQUIRE(d.ReleaseMutant(mutant, b, false, nullptr, &not_owned).wakes.empty());
  REQUIRE(not_owned);
  REQUIRE(d.ReleaseMutant(mutant, a).wakes.empty());
  REQUIRE(State(d, mutant) == 0);
  Effects passed = d.ReleaseMutant(mutant, a);
  REQUIRE(passed.wakes == std::vector<Wake>{{b, kStatusWait0}});
  REQUIRE(Info(d, mutant).owner == b);
  REQUIRE(State(d, mutant) == 0);
}

TEST_CASE("A mutant created owned belongs to its creator", "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread(), b = d.NewThread();
  ObjectId mutant = d.NewMutant(a);
  REQUIRE(State(d, mutant) == 0);
  REQUIRE(d.BeginWait(b, Wait({mutant}), 0).blocked);
  REQUIRE(d.ReleaseMutant(mutant, a).wakes ==
          std::vector<Wake>{{b, kStatusWait0}});
}

TEST_CASE("A terminated owner abandons its mutants", "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  ObjectId mutant = d.NewMutant(kNoThread);
  ObjectId other = d.NewEvent(true, false);
  d.BeginWait(a, Wait({mutant}), 0);
  d.BeginWait(a, Wait({mutant}), 0);
  // b waits on something else first, then the mutant.
  d.BeginWait(b, Wait({other, mutant}), 0);
  Effects ended = d.ThreadTerminated(a);
  REQUIRE(ended.wakes == std::vector<Wake>{{b, kStatusAbandonedWait0 + 1}});
  REQUIRE(Info(d, mutant).owner == b);
  // The abandonment is reported once.
  d.ReleaseMutant(mutant, b);
  REQUIRE(d.BeginWait(c, Wait({mutant}), 0).status == kStatusWait0);
}

TEST_CASE("Wait-any takes the first signaled object in list order",
          "[dispatch]") {
  Dispatcher d;
  ObjectId x = d.NewEvent(false, false), y = d.NewEvent(false, true),
           z = d.NewEvent(false, true);
  ThreadId a = d.NewThread();
  WaitResult result = d.BeginWait(a, Wait({x, y, z}), 0);
  REQUIRE(!result.blocked);
  REQUIRE(result.status == kStatusWait0 + 1);
  // Only y was taken.
  REQUIRE(State(d, y) == 0);
  REQUIRE(State(d, z) == 1);
}

TEST_CASE("Wait-all waits for every object and takes them together",
          "[dispatch]") {
  Dispatcher d;
  ObjectId x = d.NewEvent(false, false), y = d.NewSemaphore(1, 1);
  ThreadId a = d.NewThread(), b = d.NewThread();
  REQUIRE(d.BeginWait(a, Wait({x, y}, WaitType::kAll), 0).blocked);
  // y alone isn't enough, and isn't taken.
  REQUIRE(State(d, y) == 1);
  // Another thread takes y meanwhile.
  REQUIRE(!d.BeginWait(b, Wait({y}), 0).blocked);
  REQUIRE(d.SetEvent(x).wakes.empty());
  REQUIRE(State(d, x) == 1);
  Effects both = d.ReleaseSemaphore(y, 1);
  REQUIRE(both.wakes == std::vector<Wake>{{a, kStatusWait0}});
  REQUIRE(State(d, x) == 0);
  REQUIRE(State(d, y) == 0);
}

TEST_CASE("Deadlines poll, time out and order the expiries", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  // A deadline that has passed polls.
  WaitResult poll = d.BeginWait(a, Wait({event}, WaitType::kAny, 100), 100);
  REQUIRE(!poll.blocked);
  REQUIRE(poll.status == kStatusTimeout);
  REQUIRE(d.BeginWait(a, Wait({event}, WaitType::kAny, 500), 100).blocked);
  REQUIRE(d.BeginWait(b, Wait({event}, WaitType::kAny, 300), 100).blocked);
  REQUIRE(d.BeginWait(c, Wait({event}), 100).blocked);
  REQUIRE(d.NextDeadline() == 300);
  REQUIRE(d.Expire(299).wakes.empty());
  REQUIRE(d.Expire(600).wakes ==
          std::vector<Wake>{{b, kStatusTimeout}, {a, kStatusTimeout}});
  REQUIRE(!d.NextDeadline());
  // A timed-out waiter is gone from the queue: the set goes to c.
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{c, kStatusWait0}});
}

TEST_CASE("A user APC ends an alertable wait, or the next one", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.BeginWait(a, Wait({event}, WaitType::kAny, std::nullopt, true), 0);
  d.BeginWait(b, Wait({event}), 0);
  REQUIRE(d.QueueUserApc(a).wakes == std::vector<Wake>{{a, kStatusUserApc}});
  // Not alertable: it keeps waiting; the APC waits for an alertable wait.
  REQUIRE(d.QueueUserApc(b).wakes.empty());
  REQUIRE(Thread(d, b).waiting);
  d.UserApcsDelivered(a);
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{b, kStatusWait0}});
  WaitResult pending =
      d.BeginWait(b, Wait({event}, WaitType::kAny, std::nullopt, true), 0);
  REQUIRE(!pending.blocked);
  REQUIRE(pending.status == kStatusUserApc);
  d.UserApcsDelivered(b);
  REQUIRE(!Thread(d, b).user_apc_pending);
}

TEST_CASE("A delay is a wait on nothing", "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread(), b = d.NewThread();
  REQUIRE(d.BeginWait(a, Wait({}, WaitType::kAny, 100), 0).blocked);
  REQUIRE(d.BeginWait(b, Wait({}, WaitType::kAny, 100, true), 0).blocked);
  REQUIRE(d.Expire(99).wakes.empty());
  // Alertable: a user APC ends it early.
  REQUIRE(d.QueueUserApc(b).wakes == std::vector<Wake>{{b, kStatusUserApc}});
  REQUIRE(d.Expire(100).wakes == std::vector<Wake>{{a, kStatusTimeout}});
}

TEST_CASE("A thread object signals when its thread ends", "[dispatch]") {
  Dispatcher d;
  ThreadId worker = d.NewThread(), joiner = d.NewThread();
  ObjectId handle = d.NewThreadObject(worker);
  ObjectId never = d.NewEvent(true, false);
  d.BeginWait(worker, Wait({never}), 0);
  REQUIRE(d.BeginWait(joiner, Wait({handle}), 0).blocked);
  Effects ended = d.ThreadTerminated(worker);
  // The ending thread gets no wake; the joiner does.
  REQUIRE(ended.wakes == std::vector<Wake>{{joiner, kStatusWait0}});
  REQUIRE(Info(d, never).waiters == 0);
  REQUIRE(d.BeginWait(joiner, Wait({handle}), 0).status == kStatusWait0);
}

TEST_CASE("A guest write to a header is read back in", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread();
  d.BeginWait(a, Wait({event}), 0);
  REQUIRE(d.SetSignalState(event, 0).wakes.empty());
  REQUIRE(d.SetSignalState(event, 1).wakes ==
          std::vector<Wake>{{a, kStatusWait0}});
  REQUIRE(State(d, event) == 0);
}

TEST_CASE("A suspended waiter takes no signal until resumed", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  d.BeginWait(a, Wait({event}), 0);
  d.BeginWait(b, Wait({event}), 0);
  uint32_t previous = 99;
  d.Suspend(a, &previous);
  REQUIRE(previous == 0);
  REQUIRE(Thread(d, a).held);
  // The set goes past a to b.
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{b, kStatusWait0}});
  d.BeginWait(c, Wait({event}), 0);
  // Resumed, a waits again behind c.
  REQUIRE(d.Resume(a, 0, &previous).wakes.empty());
  REQUIRE(previous == 1);
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{c, kStatusWait0}});
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{a, kStatusWait0}});
}

TEST_CASE("Resume ends a held wait as it would begin then", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, false);
  ThreadId a = d.NewThread(), b = d.NewThread(), c = d.NewThread();
  d.BeginWait(a, Wait({event}), 0);
  d.BeginWait(b, Wait({event}, WaitType::kAny, 100), 0);
  d.BeginWait(c, Wait({event}, WaitType::kAny, std::nullopt, true), 0);
  d.Suspend(a);
  d.Suspend(b);
  d.Suspend(c);
  // Held: no wake from a signal, a deadline or an APC.
  REQUIRE(d.SetEvent(event).wakes.empty());
  REQUIRE(d.Expire(200).wakes.empty());
  REQUIRE(!d.NextDeadline());
  REQUIRE(d.QueueUserApc(c).wakes.empty());
  REQUIRE(d.Resume(a, 200).wakes == std::vector<Wake>{{a, kStatusWait0}});
  d.ResetEvent(event);
  REQUIRE(d.Resume(b, 200).wakes == std::vector<Wake>{{b, kStatusTimeout}});
  REQUIRE(d.Resume(c, 200).wakes == std::vector<Wake>{{c, kStatusUserApc}});
}

TEST_CASE("Suspension counts, and only the last resume counts", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, true);
  ThreadId a = d.NewThread();
  for (uint32_t i = 0; i < kMaxSuspendCount; ++i) {
    d.Suspend(a);
  }
  bool exceeded = false;
  uint32_t previous = 0;
  d.Suspend(a, &previous, &exceeded);
  REQUIRE(exceeded);
  REQUIRE(previous == kMaxSuspendCount);
  // A suspended thread's wait begins held, though the event is set.
  REQUIRE(d.BeginWait(a, Wait({event}), 0).blocked);
  for (uint32_t i = 1; i < kMaxSuspendCount; ++i) {
    REQUIRE(d.Resume(a, 0).wakes.empty());
  }
  REQUIRE(d.Resume(a, 0).wakes == std::vector<Wake>{{a, kStatusWait0}});
  // Resuming a thread that isn't suspended changes nothing.
  REQUIRE(d.Resume(a, 0, &previous).wakes.empty());
  REQUIRE(previous == 0);
  REQUIRE(Thread(d, a).suspend_count == 0);
}

TEST_CASE("A hold blocks a suspended thread until the last resume",
          "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread();
  REQUIRE(!d.BeginHold(a).blocked);
  d.Suspend(a);
  REQUIRE(d.BeginHold(a).blocked);
  REQUIRE(Thread(d, a).held);
  d.Suspend(a);
  REQUIRE(d.Resume(a, 0).wakes.empty());
  REQUIRE(d.Resume(a, 0).wakes == std::vector<Wake>{{a, kStatusWait0}});
  REQUIRE(!Thread(d, a).waiting);
}

TEST_CASE("Destroying a waiting thread leaves no trace", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(false, false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.BeginWait(a, Wait({event}), 0);
  d.BeginWait(b, Wait({event}), 0);
  d.DestroyThread(a);
  REQUIRE(Info(d, event).waiters == 1);
  REQUIRE(d.SetEvent(event).wakes == std::vector<Wake>{{b, kStatusWait0}});
}

TEST_CASE("A timer is unsignaled and disarmed until set", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  REQUIRE(State(d, t) == 0);
  REQUIRE(!Timer(d, t).armed);
}

TEST_CASE("A notification timer expires at its due time and wakes every waiter",
          "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.BeginWait(a, Wait({t}), 0);
  d.BeginWait(b, Wait({t}), 0);
  d.SetTimer(t, 100, 0, {});
  REQUIRE(d.Tick(99).wakes.empty());
  REQUIRE(d.Tick(100).wakes ==
          std::vector<Wake>{{a, kStatusWait0}, {b, kStatusWait0}});
  REQUIRE(State(d, t) == 1);
  REQUIRE(!Timer(d, t).armed);
}

TEST_CASE("A synchronization timer wakes one waiter and clears", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.BeginWait(a, Wait({t}), 0);
  d.BeginWait(b, Wait({t}), 0);
  d.SetTimer(t, 100, 0, {});
  REQUIRE(d.Tick(100).wakes == std::vector<Wake>{{a, kStatusWait0}});
  REQUIRE(State(d, t) == 0);
}

TEST_CASE("Re-arming a timer replaces its due time and clears it",
          "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 100, 0, {});
  d.Tick(100);
  REQUIRE(State(d, t) == 1);
  bool was = true;
  Effects set = d.SetTimer(t, 300, 0, {}, &was);
  REQUIRE(!was);
  REQUIRE(set.writes == std::vector<SignalWrite>{{t, 0}});
  d.SetTimer(t, 200, 0, {}, &was);
  REQUIRE(was);
  Effects expired = d.Tick(250);
  REQUIRE(expired.writes == std::vector<SignalWrite>{{t, 1}});
  Effects none = d.Tick(300);
  REQUIRE(none.wakes.empty());
  REQUIRE(none.writes.empty());
  REQUIRE(none.timer_apcs.empty());
}

TEST_CASE("Cancelling a timer disarms it and keeps its state", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 100, 0, {});
  bool was = false;
  d.CancelTimer(t, &was);
  REQUIRE(was);
  Effects none = d.Tick(200);
  REQUIRE(none.wakes.empty());
  REQUIRE(none.writes.empty());
  REQUIRE(State(d, t) == 0);
  d.CancelTimer(t, &was);
  REQUIRE(!was);
  d.SetTimer(t, 300, 0, {});
  d.Tick(300);
  d.CancelTimer(t, &was);
  REQUIRE(!was);
  REQUIRE(State(d, t) == 1);
}

TEST_CASE("A periodic timer expires every period", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 100, 50, {});
  for (uint64_t now : {100, 150, 200}) {
    REQUIRE(d.Tick(now).writes == std::vector<SignalWrite>{{t, 1}});
  }
  REQUIRE(Timer(d, t).due == 250);
  REQUIRE(Timer(d, t).armed);
}

TEST_CASE("A late periodic timer fires once and keeps its phase",
          "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 100, 50, {});
  REQUIRE(d.Tick(260).writes.size() == 1);
  REQUIRE(Timer(d, t).due == 310);
  ObjectId u = d.NewTimer(true);
  d.SetTimer(u, 100, 50, {});
  d.Tick(101);
  REQUIRE(Timer(d, u).due == 150);
}

TEST_CASE("Timers expire in due order, ties in arming order", "[dispatch]") {
  Dispatcher d;
  ThreadId a = d.NewThread();
  ObjectId first = d.NewTimer(true), second = d.NewTimer(true),
           third = d.NewTimer(true);
  d.SetTimer(first, 200, 0, TimerRoutine{a, 1, 0});
  d.SetTimer(second, 100, 0, TimerRoutine{a, 2, 0});
  d.SetTimer(third, 100, 0, TimerRoutine{a, 3, 0});
  Effects tick = d.Tick(200);
  REQUIRE(tick.timer_apcs == std::vector<TimerApc>{{second, a, 2, 0, 200},
                                                   {third, a, 3, 0, 200},
                                                   {first, a, 1, 0, 200}});
}

TEST_CASE("A timer routine is queued to its thread with the expiry time",
          "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, false);
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread();
  d.BeginWait(a, Wait({event}, WaitType::kAny, std::nullopt, true), 0);
  d.SetTimer(t, 100, 0, TimerRoutine{a, 0x82000000, 7});
  Effects tick = d.Tick(100);
  REQUIRE(tick.timer_apcs == std::vector<TimerApc>{{t, a, 0x82000000, 7, 100}});
  REQUIRE(tick.wakes == std::vector<Wake>{{a, kStatusUserApc}});
}

TEST_CASE("A timer routine is dropped when its thread terminates",
          "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread();
  d.SetTimer(t, 100, 0, TimerRoutine{a, 0x82000000, 7});
  d.ThreadTerminated(a);
  REQUIRE(Timer(d, t).routine == std::nullopt);
  REQUIRE(Timer(d, t).armed);
  Effects tick = d.Tick(100);
  REQUIRE(tick.timer_apcs.empty());
  REQUIRE(State(d, t) == 1);
}

TEST_CASE("Destroying an armed timer disarms it", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 100, 0, {});
  d.DestroyObject(t);
  Effects none = d.Tick(100);
  REQUIRE(none.wakes.empty());
  REQUIRE(none.writes.empty());
  REQUIRE(none.timer_apcs.empty());
  REQUIRE(d.QueryTimer(t) == std::nullopt);
}

TEST_CASE("A timer due before it was armed expires at the next tick",
          "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  d.SetTimer(t, 50, 0, {});
  REQUIRE(d.Tick(100).writes == std::vector<SignalWrite>{{t, 1}});
  REQUIRE(d.Tick(101).writes.empty());
}

TEST_CASE("A periodic timer queues one routine until it is taken",
          "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, false);
  ObjectId t = d.NewTimer(false);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.SetTimer(t, 100, 50, TimerRoutine{a, 0x82000000, 7});
  Effects first = d.Tick(100);
  REQUIRE(first.timer_apcs ==
          std::vector<TimerApc>{{t, a, 0x82000000, 7, 100}});
  // Consumed by a wait, so the next expiry has a waiter to wake.
  REQUIRE(!d.BeginWait(b, Wait({t}), 100).blocked);
  REQUIRE(d.BeginWait(b, Wait({t}), 100).blocked);
  d.UserApcsDelivered(a);
  REQUIRE(d.BeginWait(a, Wait({event}, WaitType::kAny, std::nullopt, true), 100)
              .blocked);
  // Still queued: the timer signals and wakes, but queues no second routine
  // and leaves its thread's APC state alone.
  Effects second = d.Tick(150);
  REQUIRE(second.timer_apcs.empty());
  REQUIRE(second.wakes == std::vector<Wake>{{b, kStatusWait0}});
  REQUIRE(Thread(d, a).waiting);
  REQUIRE(!Thread(d, a).user_apc_pending);
  d.TimerRoutinesTaken(first.timer_apcs);
  Effects third = d.Tick(200);
  REQUIRE(third.timer_apcs ==
          std::vector<TimerApc>{{t, a, 0x82000000, 7, 200}});
  REQUIRE(third.wakes == std::vector<Wake>{{a, kStatusUserApc}});
}

TEST_CASE("Cancelling a timer drops its queued routine", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread();
  d.SetTimer(t, 100, 50, TimerRoutine{a, 0x82000000, 7});
  d.Tick(100);
  REQUIRE(d.CancelTimer(t).withdrawn_routines ==
          std::vector<WithdrawnRoutine>{{a, t}});
  // Nothing is queued now: cancelling again withdraws nothing, and the next
  // expiry queues a routine again.
  REQUIRE(d.CancelTimer(t).withdrawn_routines.empty());
  REQUIRE(d.SetTimer(t, 200, 0, TimerRoutine{a, 0x82000000, 7})
              .withdrawn_routines.empty());
  REQUIRE(d.Tick(200).timer_apcs.size() == 1);
}

TEST_CASE("Re-arming a timer drops its queued routine", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread(), b = d.NewThread();
  d.SetTimer(t, 100, 0, TimerRoutine{a, 0x82000000, 7});
  d.Tick(100);
  Effects set = d.SetTimer(t, 200, 0, TimerRoutine{b, 0x82000010, 8});
  REQUIRE(set.withdrawn_routines == std::vector<WithdrawnRoutine>{{a, t}});
  REQUIRE(d.Tick(200).timer_apcs ==
          std::vector<TimerApc>{{t, b, 0x82000010, 8, 200}});
}

TEST_CASE("Destroying a timer drops its queued routine", "[dispatch]") {
  Dispatcher d;
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread();
  d.SetTimer(t, 100, 0, TimerRoutine{a, 0x82000000, 7});
  d.Tick(100);
  REQUIRE(d.DestroyObject(t).withdrawn_routines ==
          std::vector<WithdrawnRoutine>{{a, t}});
  ObjectId u = d.NewTimer(true);
  d.SetTimer(u, 100, 0, TimerRoutine{a, 0x82000000, 7});
  REQUIRE(d.DestroyObject(u).withdrawn_routines.empty());
}

TEST_CASE("A held thread's alertable wait ends with a timer routine on resume",
          "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, false);
  ObjectId t = d.NewTimer(true);
  ThreadId a = d.NewThread();
  d.BeginWait(a, Wait({event}, WaitType::kAny, std::nullopt, true), 0);
  d.Suspend(a);
  d.SetTimer(t, 100, 0, TimerRoutine{a, 0x82000000, 7});
  Effects tick = d.Tick(100);
  REQUIRE(tick.timer_apcs.size() == 1);
  REQUIRE(tick.wakes.empty());
  REQUIRE(Thread(d, a).user_apc_pending);
  REQUIRE(Thread(d, a).waiting);
  REQUIRE(d.Resume(a, 100).wakes == std::vector<Wake>{{a, kStatusUserApc}});
}

TEST_CASE("Timer calls leave other objects alone", "[dispatch]") {
  Dispatcher d;
  ObjectId event = d.NewEvent(true, true);
  bool was = true;
  REQUIRE(d.SetTimer(event, 100, 0, {}, &was).writes.empty());
  REQUIRE(!was);
  was = true;
  d.CancelTimer(event, &was);
  REQUIRE(!was);
  REQUIRE(d.Tick(100).writes.empty());
  REQUIRE(State(d, event) == 1);
}

}  // namespace test
}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
