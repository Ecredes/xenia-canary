/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/dispatch/dispatcher.h"

#include <algorithm>
#include <utility>

namespace xe {
namespace kernel {
namespace dispatch {

ObjectId Dispatcher::AddObject(Object object) {
  ObjectId id = next_object_++;
  objects_.emplace(id, std::move(object));
  return id;
}

ObjectId Dispatcher::NewEvent(bool notification, bool signaled) {
  Object event;
  event.kind = notification ? ObjectKind::kNotificationEvent
                            : ObjectKind::kSynchronizationEvent;
  event.signal_state = signaled ? 1 : 0;
  return AddObject(std::move(event));
}

ObjectId Dispatcher::NewSemaphore(int32_t count, int32_t limit) {
  Object semaphore;
  semaphore.kind = ObjectKind::kSemaphore;
  semaphore.signal_state = count;
  semaphore.limit = limit;
  return AddObject(std::move(semaphore));
}

ObjectId Dispatcher::NewMutant(ThreadId owner) {
  Object mutant;
  mutant.kind = ObjectKind::kMutant;
  mutant.signal_state = owner == kNoThread ? 1 : 0;
  mutant.owner = owner;
  return AddObject(std::move(mutant));
}

ObjectId Dispatcher::NewThreadObject(ThreadId thread) {
  Object object;
  object.kind = ObjectKind::kThread;
  object.thread = thread;
  return AddObject(std::move(object));
}

ObjectId Dispatcher::NewTimer(bool notification) {
  Object timer;
  timer.kind = notification ? ObjectKind::kNotificationTimer
                            : ObjectKind::kSynchronizationTimer;
  return AddObject(std::move(timer));
}

Effects Dispatcher::DestroyObject(ObjectId object) {
  Effects effects;
  auto found = objects_.find(object);
  if (found == objects_.end()) {
    return effects;
  }
  WithdrawRoutine(object, found->second, effects);
  objects_.erase(found);
  return effects;
}

ThreadId Dispatcher::NewThread() {
  ThreadId id = next_thread_++;
  threads_.emplace(id, Thread());
  return id;
}

void Dispatcher::DestroyThread(ThreadId thread) {
  auto it = threads_.find(thread);
  if (it == threads_.end()) {
    return;
  }
  for (ObjectId id : it->second.objects) {
    auto object = objects_.find(id);
    if (object != objects_.end()) {
      std::erase(object->second.waiters, thread);
    }
  }
  threads_.erase(it);
  // The shell forgets the waiter's routines with the waiter.
  Effects dropped;
  DropTimerRoutines(thread, dropped);
}

void Dispatcher::DropTimerRoutines(ThreadId thread, Effects& effects) {
  for (auto& [id, object] : objects_) {
    if (object.routine && object.routine->thread == thread) {
      WithdrawRoutine(id, object, effects);
      object.routine.reset();
    }
  }
}

void Dispatcher::WithdrawRoutine(ObjectId id, Object& timer, Effects& effects) {
  if (timer.routine_queued && timer.routine) {
    effects.withdrawn_routines.push_back({timer.routine->thread, id});
  }
  timer.routine_queued = false;
}

void Dispatcher::NoteSignal(ObjectId id, Effects& effects) {
  int32_t state = objects_.at(id).signal_state;
  auto write =
      std::find_if(effects.writes.begin(), effects.writes.end(),
                   [id](const SignalWrite& w) { return w.object == id; });
  if (write == effects.writes.end()) {
    effects.writes.push_back({id, state});
  } else {
    write->signal_state = state;
  }
}

bool Dispatcher::SignaledFor(const Object& object, ThreadId thread) const {
  if (object.kind == ObjectKind::kMutant) {
    // The owner reacquires recursively.
    return object.signal_state > 0 || object.owner == thread;
  }
  return object.signal_state > 0;
}

bool Dispatcher::Consume(ObjectId id, ThreadId thread, Effects& effects) {
  Object& object = objects_.at(id);
  switch (object.kind) {
    case ObjectKind::kSynchronizationEvent:
    case ObjectKind::kSynchronizationTimer:
      object.signal_state = 0;
      NoteSignal(id, effects);
      return false;
    case ObjectKind::kSemaphore:
      --object.signal_state;
      NoteSignal(id, effects);
      return false;
    case ObjectKind::kMutant: {
      --object.signal_state;
      object.owner = thread;
      bool abandoned = object.abandoned;
      object.abandoned = false;
      NoteSignal(id, effects);
      return abandoned;
    }
    case ObjectKind::kNotificationEvent:
    case ObjectKind::kNotificationTimer:
    case ObjectKind::kThread:
      return false;
  }
  return false;
}

std::optional<uint32_t> Dispatcher::TrySatisfy(ThreadId id, Thread& thread,
                                               Effects& effects) {
  if (thread.type == WaitType::kAny) {
    for (size_t i = 0; i < thread.objects.size(); ++i) {
      auto object = objects_.find(thread.objects[i]);
      if (object == objects_.end() || !SignaledFor(object->second, id)) {
        continue;
      }
      bool abandoned = Consume(object->first, id, effects);
      return (abandoned ? kStatusAbandonedWait0 : kStatusWait0) + uint32_t(i);
    }
    return std::nullopt;
  }
  for (ObjectId object_id : thread.objects) {
    auto object = objects_.find(object_id);
    if (object == objects_.end() || !SignaledFor(object->second, id)) {
      return std::nullopt;
    }
  }
  std::optional<uint32_t> abandoned_index;
  for (size_t i = 0; i < thread.objects.size(); ++i) {
    if (Consume(thread.objects[i], id, effects) && !abandoned_index) {
      abandoned_index = uint32_t(i);
    }
  }
  return abandoned_index ? kStatusAbandonedWait0 + *abandoned_index
                         : kStatusWait0;
}

std::optional<uint32_t> Dispatcher::TryFinish(ThreadId id, Thread& thread,
                                              uint64_t now, Effects& effects) {
  if (thread.alertable && thread.user_apc_pending) {
    return kStatusUserApc;
  }
  if (std::optional<uint32_t> status = TrySatisfy(id, thread, effects)) {
    return status;
  }
  if (thread.deadline && *thread.deadline <= now) {
    return kStatusTimeout;
  }
  return std::nullopt;
}

void Dispatcher::Enqueue(ThreadId id, const Thread& thread) {
  for (ObjectId object_id : thread.objects) {
    auto object = objects_.find(object_id);
    if (object != objects_.end()) {
      object->second.waiters.push_back(id);
    }
  }
}

void Dispatcher::Dequeue(ThreadId id, const Thread& thread) {
  for (ObjectId object_id : thread.objects) {
    auto object = objects_.find(object_id);
    if (object != objects_.end()) {
      std::erase(object->second.waiters, id);
    }
  }
}

void Dispatcher::EndWait(ThreadId id, Thread& thread, uint32_t status,
                         Effects& effects) {
  Dequeue(id, thread);
  thread.waiting = false;
  thread.held = false;
  thread.holding = false;
  thread.objects.clear();
  thread.deadline.reset();
  effects.wakes.push_back({id, status});
}

void Dispatcher::WakeWaiters(ObjectId object_id, Effects& effects) {
  auto object = objects_.find(object_id);
  if (object == objects_.end()) {
    return;
  }
  // Satisfying one waiter changes the queue: walk a copy, in arrival order.
  std::vector<ThreadId> waiters(object->second.waiters.begin(),
                                object->second.waiters.end());
  for (ThreadId id : waiters) {
    auto thread = threads_.find(id);
    if (thread == threads_.end() || !thread->second.waiting) {
      continue;
    }
    if (std::optional<uint32_t> status =
            TrySatisfy(id, thread->second, effects)) {
      EndWait(id, thread->second, *status, effects);
    }
  }
}

Effects Dispatcher::SetEvent(ObjectId event, int32_t* previous) {
  Effects effects;
  auto found = objects_.find(event);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (previous) {
    *previous = object.signal_state;
  }
  object.signal_state = 1;
  NoteSignal(event, effects);
  WakeWaiters(event, effects);
  return effects;
}

Effects Dispatcher::PulseEvent(ObjectId event, int32_t* previous) {
  Effects effects;
  auto found = objects_.find(event);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (previous) {
    *previous = object.signal_state;
  }
  object.signal_state = 1;
  WakeWaiters(event, effects);
  object.signal_state = 0;
  NoteSignal(event, effects);
  return effects;
}

Effects Dispatcher::ResetEvent(ObjectId event, int32_t* previous) {
  Effects effects;
  auto found = objects_.find(event);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (previous) {
    *previous = object.signal_state;
  }
  object.signal_state = 0;
  NoteSignal(event, effects);
  return effects;
}

Effects Dispatcher::ReleaseSemaphore(ObjectId semaphore, int32_t count,
                                     int32_t* previous, bool* exceeded) {
  Effects effects;
  auto found = objects_.find(semaphore);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (previous) {
    *previous = object.signal_state;
  }
  bool over = count <= 0 ||
              int64_t(object.signal_state) + count > int64_t(object.limit);
  if (exceeded) {
    *exceeded = over;
  }
  if (over) {
    return effects;
  }
  object.signal_state += count;
  NoteSignal(semaphore, effects);
  WakeWaiters(semaphore, effects);
  return effects;
}

Effects Dispatcher::ReleaseMutant(ObjectId mutant, ThreadId caller,
                                  bool abandon, int32_t* previous,
                                  bool* not_owned) {
  Effects effects;
  auto found = objects_.find(mutant);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (previous) {
    *previous = object.signal_state;
  }
  bool owned = object.owner == caller && object.signal_state <= 0;
  if (not_owned) {
    *not_owned = !abandon && !owned;
  }
  if (!abandon && !owned) {
    return effects;
  }
  if (abandon) {
    object.signal_state = 1;
    object.abandoned = true;
  } else {
    ++object.signal_state;
  }
  if (object.signal_state > 0) {
    object.owner = kNoThread;
  }
  NoteSignal(mutant, effects);
  WakeWaiters(mutant, effects);
  return effects;
}

Effects Dispatcher::SetSignalState(ObjectId id, int32_t signal_state) {
  Effects effects;
  auto found = objects_.find(id);
  if (found == objects_.end()) {
    return effects;
  }
  Object& object = found->second;
  if (object.signal_state == signal_state) {
    return effects;
  }
  object.signal_state = signal_state;
  if (object.kind == ObjectKind::kMutant && signal_state > 0) {
    object.owner = kNoThread;
  }
  NoteSignal(id, effects);
  WakeWaiters(id, effects);
  return effects;
}

Effects Dispatcher::SetTimer(ObjectId id, uint64_t due, uint64_t period,
                             std::optional<TimerRoutine> routine,
                             bool* was_armed) {
  Effects effects;
  auto found = objects_.find(id);
  if (found == objects_.end()) {
    return effects;
  }
  Object& timer = found->second;
  if (was_armed) {
    *was_armed = timer.armed;
  }
  WithdrawRoutine(id, timer, effects);
  timer.armed = true;
  timer.due = due;
  timer.period = period;
  timer.routine = routine;
  timer.sequence = next_sequence_++;
  timer.signal_state = 0;
  NoteSignal(id, effects);
  return effects;
}

Effects Dispatcher::CancelTimer(ObjectId id, bool* was_armed) {
  Effects effects;
  if (was_armed) {
    *was_armed = false;
  }
  auto found = objects_.find(id);
  if (found == objects_.end()) {
    return effects;
  }
  if (was_armed) {
    *was_armed = found->second.armed;
  }
  found->second.armed = false;
  WithdrawRoutine(id, found->second, effects);
  return effects;
}

void Dispatcher::TimerRoutinesTaken(std::span<const TimerApc> apcs) {
  for (const TimerApc& apc : apcs) {
    auto found = objects_.find(apc.timer);
    if (found != objects_.end()) {
      found->second.routine_queued = false;
    }
  }
}

Effects Dispatcher::Tick(uint64_t now) {
  Effects effects;
  struct Due {
    uint64_t due;
    uint64_t sequence;
    ObjectId id;
  };
  std::vector<Due> expiring;
  for (const auto& [id, object] : objects_) {
    if (object.armed && object.due <= now) {
      expiring.push_back({object.due, object.sequence, id});
    }
  }
  std::sort(expiring.begin(), expiring.end(), [](const Due& a, const Due& b) {
    return a.due != b.due ? a.due < b.due : a.sequence < b.sequence;
  });
  for (const Due& next : expiring) {
    Object& timer = objects_.at(next.id);
    if (timer.period == 0) {
      timer.armed = false;
    } else {
      // TODO(Ecredes): guest reinsertion rule unobserved; keeps phase at tick
      // granularity.
      timer.due += timer.period;
      if (timer.due <= now) {
        timer.due = now + timer.period;
      }
    }
    timer.signal_state = 1;
    NoteSignal(next.id, effects);
    WakeWaiters(next.id, effects);
    // TODO(Ecredes): the guest's APC rule for timers is unobserved; follows NT
    // (one queued routine per timer).
    if (timer.routine && !timer.routine_queued &&
        threads_.count(timer.routine->thread)) {
      timer.routine_queued = true;
      effects.timer_apcs.push_back({next.id, timer.routine->thread,
                                    timer.routine->routine,
                                    timer.routine->argument, now});
      Effects apc = QueueUserApc(timer.routine->thread);
      effects.wakes.insert(effects.wakes.end(), apc.wakes.begin(),
                           apc.wakes.end());
    }
  }
  return effects;
}

std::optional<TimerInfo> Dispatcher::QueryTimer(ObjectId id) const {
  auto it = objects_.find(id);
  if (it == objects_.end() ||
      (it->second.kind != ObjectKind::kNotificationTimer &&
       it->second.kind != ObjectKind::kSynchronizationTimer)) {
    return std::nullopt;
  }
  const Object& timer = it->second;
  return TimerInfo{timer.armed, timer.due, timer.period, timer.routine};
}

WaitResult Dispatcher::BeginWait(ThreadId id, const WaitRequest& request,
                                 uint64_t now) {
  WaitResult result;
  auto found = threads_.find(id);
  if (found == threads_.end()) {
    return result;
  }
  Thread& thread = found->second;
  thread.type = request.type;
  thread.objects = request.objects;
  thread.deadline = request.deadline;
  thread.alertable = request.alertable;
  result.blocked = true;
  if (thread.suspend_count) {
    // It can't run: the wait is decided at the last Resume.
    thread.waiting = true;
    thread.held = true;
    return result;
  }
  if (std::optional<uint32_t> status =
          TryFinish(id, thread, now, result.effects)) {
    thread.objects.clear();
    thread.deadline.reset();
    result.blocked = false;
    result.status = *status;
    return result;
  }
  thread.waiting = true;
  Enqueue(id, thread);
  return result;
}

Effects Dispatcher::Expire(uint64_t now) {
  Effects effects;
  std::vector<std::pair<uint64_t, ThreadId>> due;
  for (const auto& [id, thread] : threads_) {
    if (thread.waiting && !thread.held && thread.deadline &&
        *thread.deadline <= now) {
      due.emplace_back(*thread.deadline, id);
    }
  }
  std::sort(due.begin(), due.end());
  for (const auto& [deadline, id] : due) {
    EndWait(id, threads_.at(id), kStatusTimeout, effects);
  }
  return effects;
}

std::optional<uint64_t> Dispatcher::NextDeadline() const {
  std::optional<uint64_t> next;
  for (const auto& [id, thread] : threads_) {
    if (thread.waiting && !thread.held && thread.deadline &&
        (!next || *thread.deadline < *next)) {
      next = thread.deadline;
    }
  }
  return next;
}

Effects Dispatcher::QueueUserApc(ThreadId id) {
  Effects effects;
  auto found = threads_.find(id);
  if (found == threads_.end()) {
    return effects;
  }
  Thread& thread = found->second;
  thread.user_apc_pending = true;
  if (thread.waiting && thread.alertable && !thread.held) {
    EndWait(id, thread, kStatusUserApc, effects);
  }
  return effects;
}

void Dispatcher::UserApcsDelivered(ThreadId id) {
  auto found = threads_.find(id);
  if (found != threads_.end()) {
    found->second.user_apc_pending = false;
  }
}

Effects Dispatcher::Suspend(ThreadId id, uint32_t* previous, bool* exceeded) {
  Effects effects;
  auto found = threads_.find(id);
  if (found == threads_.end()) {
    return effects;
  }
  Thread& thread = found->second;
  if (previous) {
    *previous = thread.suspend_count;
  }
  bool over = thread.suspend_count >= kMaxSuspendCount;
  if (exceeded) {
    *exceeded = over;
  }
  if (over) {
    return effects;
  }
  if (thread.suspend_count++ == 0 && thread.waiting && !thread.holding) {
    // As a kernel APC would, it leaves the wait; Resume begins it again.
    Dequeue(id, thread);
    thread.held = true;
  }
  return effects;
}

Effects Dispatcher::Resume(ThreadId id, uint64_t now, uint32_t* previous) {
  Effects effects;
  auto found = threads_.find(id);
  if (found == threads_.end()) {
    return effects;
  }
  Thread& thread = found->second;
  if (previous) {
    *previous = thread.suspend_count;
  }
  if (thread.suspend_count == 0) {
    return effects;
  }
  --thread.suspend_count;
  if (thread.suspend_count != 0 || !thread.waiting) {
    return effects;
  }
  if (thread.holding) {
    EndWait(id, thread, kStatusWait0, effects);
    return effects;
  }
  thread.held = false;
  if (std::optional<uint32_t> status = TryFinish(id, thread, now, effects)) {
    EndWait(id, thread, *status, effects);
  } else {
    Enqueue(id, thread);
  }
  return effects;
}

WaitResult Dispatcher::BeginHold(ThreadId id) {
  WaitResult result;
  auto found = threads_.find(id);
  if (found == threads_.end() || found->second.suspend_count == 0) {
    return result;
  }
  Thread& thread = found->second;
  thread.waiting = true;
  thread.holding = true;
  thread.objects.clear();
  thread.deadline.reset();
  thread.alertable = false;
  result.blocked = true;
  return result;
}

Effects Dispatcher::ThreadTerminated(ThreadId id) {
  Effects effects;
  DropTimerRoutines(id, effects);
  auto thread = threads_.find(id);
  if (thread != threads_.end() && thread->second.waiting) {
    // It won't come back for the wake: drop it without one.
    Dequeue(id, thread->second);
    thread->second.waiting = false;
    thread->second.held = false;
    thread->second.holding = false;
    thread->second.objects.clear();
  }
  std::vector<ObjectId> owned;
  std::vector<ObjectId> thread_objects;
  for (const auto& [object_id, object] : objects_) {
    if (object.kind == ObjectKind::kMutant && object.owner == id) {
      owned.push_back(object_id);
    } else if (object.kind == ObjectKind::kThread && object.thread == id) {
      thread_objects.push_back(object_id);
    }
  }
  std::sort(owned.begin(), owned.end());
  for (ObjectId mutant : owned) {
    Effects released = ReleaseMutant(mutant, id, true);
    effects.wakes.insert(effects.wakes.end(), released.wakes.begin(),
                         released.wakes.end());
    for (const SignalWrite& write : released.writes) {
      NoteSignal(write.object, effects);
    }
  }
  for (ObjectId object_id : thread_objects) {
    objects_.at(object_id).signal_state = 1;
    NoteSignal(object_id, effects);
    WakeWaiters(object_id, effects);
  }
  return effects;
}

std::optional<ObjectInfo> Dispatcher::Query(ObjectId id) const {
  auto it = objects_.find(id);
  if (it == objects_.end()) {
    return std::nullopt;
  }
  const Object& object = it->second;
  return ObjectInfo{object.kind,  object.signal_state, object.limit,
                    object.owner, object.abandoned,    object.waiters.size()};
}

std::optional<ThreadInfo> Dispatcher::QueryThread(ThreadId id) const {
  auto it = threads_.find(id);
  if (it == threads_.end()) {
    return std::nullopt;
  }
  const Thread& thread = it->second;
  return ThreadInfo{thread.waiting,       thread.type,
                    thread.objects,       thread.deadline,
                    thread.alertable,     thread.user_apc_pending,
                    thread.suspend_count, thread.held || thread.holding};
}

}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
