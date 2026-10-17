/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/export_telemetry.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/telemetry.h"

DEFINE_string(telemetry_exports, "stub",
              "Exports whose first call is reported: stub (stub and sketchy) "
              "or all.",
              "Telemetry");

namespace xe::kernel::util {

namespace {

// Never destroyed: ExportCallsJson runs from exit handlers, possibly after
// static destruction has started.
struct Registry {
  std::mutex mutex;
  ExportStats* head = nullptr;
};

Registry& GetRegistry() {
  static Registry* registry = new Registry();
  return *registry;
}

}  // namespace

bool ShouldReportFirstCall(cpu::ExportTag::type tags, bool report_all) {
  return report_all ||
         (tags & (cpu::ExportTag::kStub | cpu::ExportTag::kSketchy)) != 0;
}

bool IsUiShowExport(const cpu::Export& entry) {
  const auto category = static_cast<cpu::ExportCategory>(
      (entry.tags >> cpu::ExportTag::CategoryShift) & 0xFF);
  return category == cpu::ExportCategory::kUI &&
         std::string_view(entry.name).starts_with("XamShow");
}

ExportStats::ExportStats(const char* library, const cpu::Export* entry)
    : library_(library), entry_(entry), is_ui_show_(IsUiShowExport(*entry)) {
  Registry& registry = GetRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  next_ = registry.head;
  if (registry.head) {
    registry.head->prev_ = this;
  }
  registry.head = this;
}

ExportStats::~ExportStats() {
  Registry& registry = GetRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (prev_) {
    prev_->next_ = next_;
  } else {
    registry.head = next_;
  }
  if (next_) {
    next_->prev_ = prev_;
  }
}

void ExportStats::ReportFirstCall(uint32_t caller_lr,
                                  uint32_t guest_thread_id) {
  const cpu::ExportTag::type tags = entry_->tags;
  if (!ShouldReportFirstCall(tags, cvars::telemetry_exports == "all")) {
    return;
  }
  const char* tag = (tags & cpu::ExportTag::kStub)      ? "stub"
                    : (tags & cpu::ExportTag::kSketchy) ? "sketchy"
                                                        : "implemented";
  telemetry::Event event("export_first_call");
  event.Str("library", library_)
      .UInt("ordinal", entry_->ordinal)
      .Str("name", entry_->name)
      .Str("tag", tag)
      .Hex("caller", caller_lr)
      .UInt("thread", guest_thread_id);
  telemetry::Emit(std::move(event));
}

void ExportStats::ReportUiShown(uint32_t user) {
  telemetry::Event event("xam_ui");
  event.Str("ui", entry_->name).UInt("user", user);
  telemetry::Emit(std::move(event));
}

std::string ExportCallsJson() {
  std::vector<std::pair<std::string, uint64_t>> entries;
  {
    Registry& registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (ExportStats* stats = registry.head; stats; stats = stats->next_) {
      uint64_t calls = stats->calls();
      if (calls) {
        entries.emplace_back(
            std::string(stats->library_) + "::" + stats->entry_->name, calls);
      }
    }
  }
  std::sort(entries.begin(), entries.end());
  std::string json = "{";
  bool first = true;
  for (const auto& [name, calls] : entries) {
    if (!first) {
      json += ',';
    }
    first = false;
    telemetry::AppendJsonString(json, name);
    json += ':';
    json += std::to_string(calls);
  }
  json += '}';
  return json;
}

void RegisterExportTelemetry() {
  static std::once_flag once;
  std::call_once(once, [] {
    telemetry::AddExitField([](telemetry::Event& event) {
      event.Raw("export_calls", ExportCallsJson());
    });
  });
}

}  // namespace xe::kernel::util
