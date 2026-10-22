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

void Dispatcher::DestroyObject(ObjectId object) { objects_.erase(object); }

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

void Dispatcher::EndWait(ThreadId id, Thread& thread, uint32_t status,
                         Effects& effects) {
  for (ObjectId object_id : thread.objects) {
    auto object = objects_.find(object_id);
    if (object != objects_.end()) {
      std::erase(object->second.waiters, id);
    }
  }
  thread.waiting = false;
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

WaitResult Dispatcher::BeginWait(ThreadId id, const WaitRequest& request,
                                 uint64_t now) {
  WaitResult result;
  auto found = threads_.find(id);
  if (found == threads_.end()) {
    return result;
  }
  Thread& thread = found->second;
  if (request.alertable && thread.user_apc_pending) {
    result.status = kStatusUserApc;
    return result;
  }
  thread.type = request.type;
  thread.objects = request.objects;
  if (std::optional<uint32_t> status = TrySatisfy(id, thread, result.effects)) {
    thread.objects.clear();
    result.status = *status;
    return result;
  }
  if (request.deadline && *request.deadline <= now) {
    thread.objects.clear();
    result.status = kStatusTimeout;
    return result;
  }
  thread.waiting = true;
  thread.deadline = request.deadline;
  thread.alertable = request.alertable;
  for (ObjectId object_id : thread.objects) {
    auto object = objects_.find(object_id);
    if (object != objects_.end()) {
      object->second.waiters.push_back(id);
    }
  }
  result.blocked = true;
  return result;
}

Effects Dispatcher::Expire(uint64_t now) {
  Effects effects;
  std::vector<std::pair<uint64_t, ThreadId>> due;
  for (const auto& [id, thread] : threads_) {
    if (thread.waiting && thread.deadline && *thread.deadline <= now) {
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
    if (thread.waiting && thread.deadline &&
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
  if (thread.waiting && thread.alertable) {
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

Effects Dispatcher::ThreadTerminated(ThreadId id) {
  Effects effects;
  auto thread = threads_.find(id);
  if (thread != threads_.end() && thread->second.waiting) {
    // It won't come back for the wake: drop it without one.
    for (ObjectId object_id : thread->second.objects) {
      auto object = objects_.find(object_id);
      if (object != objects_.end()) {
        std::erase(object->second.waiters, id);
      }
    }
    thread->second.waiting = false;
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
  return ThreadInfo{thread.waiting,  thread.type,      thread.objects,
                    thread.deadline, thread.alertable, thread.user_apc_pending};
}

}  // namespace dispatch
}  // namespace kernel
}  // namespace xe
