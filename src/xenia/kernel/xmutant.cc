/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xmutant.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XMutant::XMutant(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() = default;

void XMutant::Bind(dispatch::ThreadId owner) {
  BindDispatchObject(kernel_state()->dispatch()->Read(
      [owner](dispatch::Dispatcher& core) { return core.NewMutant(owner); }));
}

void XMutant::Initialize(bool initial_owner) {
  assert_false(dispatch_object());
  Bind(initial_owner ? kernel_state()->dispatch()->CurrentWaiter().id
                     : dispatch::kNoThread);
}

void XMutant::InitializeNative(void* native_ptr,
                               const X_DISPATCH_HEADER* header) {
  assert_false(dispatch_object());

  // Haven't seen this yet, but it's possible.
  assert_always();
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon,
                                bool wait) {
  set_priority_increment(priority_increment);
  dispatch::Shell* shell = kernel_state()->dispatch();
  dispatch::ThreadId caller = shell->CurrentWaiter().id;
  bool not_owned = false;
  shell->Run([&, this](dispatch::Dispatcher& core) {
    return core.ReleaseMutant(dispatch_object(), caller, abandon, nullptr,
                              &not_owned);
  });
  return not_owned ? X_STATUS_MUTANT_NOT_OWNED : X_STATUS_SUCCESS;
}

dispatch::Effects XMutant::SignalForWait(dispatch::Dispatcher& core,
                                         dispatch::ThreadId caller) {
  return core.ReleaseMutant(dispatch_object(), caller);
}

const XThread* XMutant::owning_thread() const {
  dispatch::Shell* shell = kernel_state()->dispatch();
  std::optional<dispatch::ObjectInfo> info =
      shell->Read([this](dispatch::Dispatcher& core) {
        return core.Query(dispatch_object());
      });
  if (!info || info->owner == dispatch::kNoThread) {
    return nullptr;
  }
  return shell->ThreadOf(info->owner);
}

bool XMutant::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  // Saving runs with the guest stopped, so the owner is still there.
  const XThread* owner = owning_thread();
  uint32_t owning_thread_handle = owner ? owner->handle() : 0;
  stream->Write<uint32_t>(owning_thread_handle);
  XELOGD("XMutant {:08X} (owner: {:08X})", handle(), owning_thread_handle);

  return true;
}

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto mutant = new XMutant();
  mutant->kernel_state_ = kernel_state;

  if (!mutant->RestoreObject(stream)) {
    delete mutant;
    return nullptr;
  }

  mutant->Initialize(false);

  auto owning_thread_handle = stream->Read<uint32_t>();
  if (owning_thread_handle) {
    auto owner = kernel_state->object_table()->LookupObject<XThread>(
        owning_thread_handle);
    if (owner) {
      owner->AcquireMutantOnStartup(retain_object(mutant));
    }
  }

  return object_ref<XMutant>(mutant);
}

}  // namespace kernel
}  // namespace xe
