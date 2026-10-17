/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"
namespace xe {
namespace kernel {
namespace shim {

thread_local StringBuffer string_buffer_;

StringBuffer* thread_local_string_buffer() { return &string_buffer_; }

XThread* ContextParam::CurrentXThread() const {
  return XThread::GetCurrentThread();
}

}  // namespace shim

// Defined here (not in export_telemetry.cc) so the export statistics stay
// linkable without the rest of the kernel.
namespace util {
const char* ExportLibraryName(shim::KernelModuleId id) {
  switch (id) {
    case shim::KernelModuleId::xboxkrnl:
      return "xboxkrnl";
    case shim::KernelModuleId::xam:
      return "xam";
    case shim::KernelModuleId::xbdm:
      return "xbdm";
  }
  return "unknown";
}

uint32_t CurrentGuestThreadId() {
  XThread* thread = XThread::TryGetCurrentThread();
  if (!thread) {
    return 0;
  }
  return thread->guest_object<X_KTHREAD>()->thread_id;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
