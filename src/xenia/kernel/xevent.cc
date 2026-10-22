/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xevent.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

XEvent::XEvent(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Bind(bool signaled) {
  BindDispatchObject(kernel_state()->dispatch()->Read(
      [this, signaled](dispatch::Dispatcher& core) {
        return core.NewEvent(manual_reset_, signaled);
      }));
}

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(dispatch_object());

  manual_reset_ = manual_reset;
  CreateNative<X_KEVENT>();
  auto* kevent = memory()->TranslateVirtual<X_KEVENT*>(guest_object());
  // Don't touch header.wait_list: SetNativePointer stashes the handle there.
  kevent->header.type = manual_reset
                            ? X_OBJECT_TYPES::EventNotificationObject
                            : X_OBJECT_TYPES::EventSynchronizationObject;
  kevent->header.signal_state = initial_state ? 1 : 0;
  Bind(initial_state);
}

void XEvent::InitializeNative(void* native_ptr,
                              const X_DISPATCH_HEADER* header) {
  assert_false(dispatch_object());

  switch (header->type) {
    case X_OBJECT_TYPES::EventNotificationObject:
      manual_reset_ = true;
      break;
    case X_OBJECT_TYPES::EventSynchronizationObject:
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);
  Bind(header->signal_state != 0);
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  TouchHeader();
  int32_t previous = 0;
  kernel_state()->dispatch()->Run(
      [this, &previous](dispatch::Dispatcher& core) {
        return core.SetEvent(dispatch_object(), &previous);
      });
  return previous;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  TouchHeader();
  int32_t previous = 0;
  kernel_state()->dispatch()->Run(
      [this, &previous](dispatch::Dispatcher& core) {
        return core.PulseEvent(dispatch_object(), &previous);
      });
  return previous;
}

int32_t XEvent::Reset() {
  TouchHeader();
  int32_t previous = 0;
  kernel_state()->dispatch()->Run(
      [this, &previous](dispatch::Dispatcher& core) {
        return core.ResetEvent(dispatch_object(), &previous);
      });
  return previous;
}

dispatch::Effects XEvent::SignalForWait(dispatch::Dispatcher& core,
                                        dispatch::ThreadId caller) {
  return core.SetEvent(dispatch_object());
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  std::optional<dispatch::ObjectInfo> info =
      kernel_state()->dispatch()->Read([this](dispatch::Dispatcher& core) {
        return core.Query(dispatch_object());
      });
  *out_type = manual_reset_ ? 0 : 1;
  *out_state = info && info->signal_state > 0 ? 1 : 0;
}

void XEvent::Clear() { Reset(); }

bool XEvent::Save(ByteStream* stream) {
  XELOGD("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  uint32_t type = 0, state = 0;
  Query(&type, &state);
  stream->Write<bool>(state != 0);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();
  evt->Bind(signaled);

  return object_ref<XEvent>(evt);
}

}  // namespace kernel
}  // namespace xe
