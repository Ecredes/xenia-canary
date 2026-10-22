/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsemaphore.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/dispatch/shell.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

XSemaphore::XSemaphore(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSemaphore::~XSemaphore() = default;

void XSemaphore::Bind(int32_t count) {
  BindDispatchObject(kernel_state()->dispatch()->Read(
      [this, count](dispatch::Dispatcher& core) {
        return core.NewSemaphore(count, int32_t(maximum_count_));
      }));
}

bool XSemaphore::Initialize(int32_t initial_count, int32_t maximum_count) {
  assert_false(dispatch_object());

  CreateNative(sizeof(X_KSEMAPHORE));
  auto* semaphore = memory()->TranslateVirtual<X_KSEMAPHORE*>(guest_object());
  semaphore->header.type = X_OBJECT_TYPES::SemaphoreObject;
  semaphore->header.signal_state = initial_count;
  semaphore->limit = maximum_count;

  maximum_count_ = maximum_count;
  Bind(initial_count);
  return true;
}

bool XSemaphore::InitializeNative(void* native_ptr,
                                  const X_DISPATCH_HEADER* header) {
  assert_false(dispatch_object());
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr));
  auto semaphore = reinterpret_cast<X_KSEMAPHORE*>(native_ptr);
  maximum_count_ = semaphore->limit;
  Bind(int32_t(uint32_t(semaphore->header.signal_state)));
  return true;
}

bool XSemaphore::ReleaseSemaphore(int32_t release_count,
                                  int32_t* out_previous_count) {
  TouchHeader();
  int32_t previous_count = 0;
  bool exceeded = false;
  kernel_state()->dispatch()->Run([&, this](dispatch::Dispatcher& core) {
    return core.ReleaseSemaphore(dispatch_object(), release_count,
                                 &previous_count, &exceeded);
  });
  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  return !exceeded;
}

dispatch::Effects XSemaphore::SignalForWait(dispatch::Dispatcher& core,
                                            dispatch::ThreadId caller) {
  return core.ReleaseSemaphore(dispatch_object(), 1);
}

bool XSemaphore::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  std::optional<dispatch::ObjectInfo> info =
      kernel_state()->dispatch()->Read([this](dispatch::Dispatcher& core) {
        return core.Query(dispatch_object());
      });
  uint32_t free_count = info ? uint32_t(std::max(info->signal_state, 0)) : 0;
  XELOGD("XSemaphore {:08X} (count {}/{})", handle(), free_count,
         maximum_count_);

  stream->Write(maximum_count_);
  stream->Write(free_count);

  return true;
}

object_ref<XSemaphore> XSemaphore::Restore(KernelState* kernel_state,
                                           ByteStream* stream) {
  auto sem = new XSemaphore(nullptr);
  sem->kernel_state_ = kernel_state;

  if (!sem->RestoreObject(stream)) {
    return nullptr;
  }

  sem->maximum_count_ = stream->Read<uint32_t>();
  auto free_count = stream->Read<uint32_t>();
  XELOGD("XSemaphore {:08X} (count {}/{})", sem->handle(), free_count,
         sem->maximum_count_);
  sem->Bind(int32_t(free_count));

  return object_ref<XSemaphore>(sem);
}

}  // namespace kernel
}  // namespace xe
