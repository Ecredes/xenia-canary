/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_EXPORT_TELEMETRY_H_
#define XENIA_KERNEL_UTIL_EXPORT_TELEMETRY_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "xenia/cpu/export_resolver.h"

namespace xe::kernel::shim {
enum class KernelModuleId;
}  // namespace xe::kernel::shim

namespace xe::kernel::util {

// "xboxkrnl", "xam" or "xbdm".
const char* ExportLibraryName(shim::KernelModuleId id);

// True for stub and sketchy exports, or for every export when report_all.
bool ShouldReportFirstCall(cpu::ExportTag::type tags, bool report_all);

// The id of the calling guest thread, or 0 when not on a guest thread.
uint32_t CurrentGuestThreadId();

// Per-export call counter. Production instances are heap-allocated by the
// export trampoline and never destroyed (the exit handlers and still-running
// guest threads use them during process exit). They register themselves in a
// process-wide list used by ExportCallsJson; the destructor unregisters, for
// tests that use short-lived instances.
class ExportStats {
 public:
  ExportStats(const char* library, const cpu::Export* entry);
  ~ExportStats();
  ExportStats(const ExportStats&) = delete;
  ExportStats& operator=(const ExportStats&) = delete;

  // Counts the call (one relaxed fetch_add). On the first call only,
  // guest_thread_id() is evaluated and export_first_call is emitted when the
  // telemetry_exports cvar selects this export.
  template <typename ThreadIdFn>
  void OnCall(uint32_t caller_lr, ThreadIdFn&& guest_thread_id) {
    if (calls_.fetch_add(1, std::memory_order_relaxed) == 0) {
      ReportFirstCall(caller_lr, guest_thread_id());
    }
  }
  uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }

 private:
  friend std::string ExportCallsJson();

  void ReportFirstCall(uint32_t caller_lr, uint32_t guest_thread_id);

  const char* library_;
  const cpu::Export* entry_;
  std::atomic<uint64_t> calls_{0};
  ExportStats* prev_ = nullptr;
  ExportStats* next_ = nullptr;
};

// {"xam::XamShowMessageBoxUI":2,...} over every registered export with calls
// > 0, sorted by name. Touches only never-destroyed storage and takes no
// telemetry locks, so it is safe from an exit-field provider.
std::string ExportCallsJson();

// Registers the exit.export_calls provider (once per process).
void RegisterExportTelemetry();

}  // namespace xe::kernel::util

#endif  // XENIA_KERNEL_UTIL_EXPORT_TELEMETRY_H_
