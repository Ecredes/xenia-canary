/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/util/export_telemetry.h"

namespace xe::kernel::util {

using cpu::Export;
using cpu::ExportTag;

TEST_CASE("ShouldReportFirstCall selects stub and sketchy", "[telemetry]") {
  CHECK(ShouldReportFirstCall(ExportTag::kStub, false));
  CHECK(ShouldReportFirstCall(ExportTag::kSketchy, false));
  CHECK(
      ShouldReportFirstCall(ExportTag::kStub | ExportTag::kImplemented, false));
  CHECK_FALSE(ShouldReportFirstCall(ExportTag::kImplemented, false));
  CHECK(ShouldReportFirstCall(ExportTag::kImplemented, true));
}

TEST_CASE("IsUnimplementedImport covers listed but unimplemented exports",
          "[telemetry]") {
  CHECK(cpu::IsUnimplementedImport(nullptr));
  // Listed in the kernel tables, no trampoline registered.
  Export listed(10, Export::Type::kFunction, "FakeListed");
  CHECK(cpu::IsUnimplementedImport(&listed));
  Export implemented(11, Export::Type::kFunction, "FakeImplemented",
                     ExportTag::kImplemented);
  implemented.function_data.trampoline = [](cpu::ppc::PPCContext*) {};
  CHECK_FALSE(cpu::IsUnimplementedImport(&implemented));
  Export no_value(12, Export::Type::kVariable, "FakeNoValue");
  CHECK(cpu::IsUnimplementedImport(&no_value));
  Export with_value(13, Export::Type::kVariable, "FakeWithValue",
                    ExportTag::kImplemented);
  CHECK_FALSE(cpu::IsUnimplementedImport(&with_value));
}

TEST_CASE("ExportCallsJson lists exports with calls", "[telemetry]") {
  cpu::ppc::PPCContext context{};
  context.lr = 0x82000000;
  Export fake_a(1, Export::Type::kFunction, "FakeA", ExportTag::kImplemented);
  Export fake_b(2, Export::Type::kFunction, "FakeB", ExportTag::kImplemented);
  ExportStats stats_a("xam", &fake_a);
  ExportStats stats_b("xam", &fake_b);
  for (int i = 0; i < 3; ++i) {
    stats_a.OnCall(context, [] { return 1u; });
  }
  CHECK(stats_a.calls() == 3);
  CHECK(stats_b.calls() == 0);
  std::string json = ExportCallsJson();
  CHECK(json.find("\"xam::FakeA\":3") != std::string::npos);
  CHECK(json.find("FakeB") == std::string::npos);
}

TEST_CASE("ExportStats unregisters on destruction", "[telemetry]") {
  {
    cpu::ppc::PPCContext context{};
    Export fake(3, Export::Type::kFunction, "FakeC", ExportTag::kImplemented);
    ExportStats stats("xboxkrnl", &fake);
    stats.OnCall(context, [] { return 0u; });
    CHECK(ExportCallsJson().find("xboxkrnl::FakeC") != std::string::npos);
  }
  CHECK(ExportCallsJson().find("FakeC") == std::string::npos);
}

TEST_CASE("OnCall evaluates the thread id only on the first call",
          "[telemetry]") {
  cpu::ppc::PPCContext context{};
  Export fake(4, Export::Type::kFunction, "FakeD", ExportTag::kImplemented);
  ExportStats stats("xam", &fake);
  int evaluations = 0;
  for (int i = 0; i < 5; ++i) {
    stats.OnCall(context, [&] {
      ++evaluations;
      return 0u;
    });
  }
  CHECK(evaluations == 1);
  CHECK(stats.calls() == 5);
}

namespace {
constexpr ExportTag::type Tags(cpu::ExportCategory category) {
  return ExportTag::kImplemented |
         (static_cast<ExportTag::type>(category) << ExportTag::CategoryShift);
}
}  // namespace

TEST_CASE("IsUiShowExport matches XamShow* in the UI category", "[telemetry]") {
  Export show(10, Export::Type::kFunction, "XamShowMessageBoxUI",
              Tags(cpu::ExportCategory::kUI));
  Export active(11, Export::Type::kFunction, "XamIsUIActive",
                Tags(cpu::ExportCategory::kUI));
  Export misc(12, Export::Type::kFunction, "XamShowFoo",
              Tags(cpu::ExportCategory::kMisc));
  CHECK(IsUiShowExport(show));
  CHECK_FALSE(IsUiShowExport(active));
  CHECK_FALSE(IsUiShowExport(misc));
}

}  // namespace xe::kernel::util
