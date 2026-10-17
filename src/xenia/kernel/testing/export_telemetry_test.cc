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
#include "xenia/kernel/util/export_telemetry.h"

namespace xe::kernel::util {

using cpu::Export;
using cpu::ExportTag;

TEST_CASE("ShouldReportFirstCall selects stub and sketchy", "[telemetry]") {
  CHECK(ShouldReportFirstCall(ExportTag::kStub, false));
  CHECK(ShouldReportFirstCall(ExportTag::kSketchy, false));
  CHECK(ShouldReportFirstCall(ExportTag::kStub | ExportTag::kImplemented,
                              false));
  CHECK_FALSE(ShouldReportFirstCall(ExportTag::kImplemented, false));
  CHECK(ShouldReportFirstCall(ExportTag::kImplemented, true));
}

TEST_CASE("ExportCallsJson lists exports with calls", "[telemetry]") {
  Export fake_a(1, Export::Type::kFunction, "FakeA", ExportTag::kImplemented);
  Export fake_b(2, Export::Type::kFunction, "FakeB", ExportTag::kImplemented);
  ExportStats stats_a("xam", &fake_a);
  ExportStats stats_b("xam", &fake_b);
  for (int i = 0; i < 3; ++i) {
    stats_a.OnCall(0x82000000, [] { return 1u; });
  }
  CHECK(stats_a.calls() == 3);
  CHECK(stats_b.calls() == 0);
  std::string json = ExportCallsJson();
  CHECK(json.find("\"xam::FakeA\":3") != std::string::npos);
  CHECK(json.find("FakeB") == std::string::npos);
}

TEST_CASE("ExportStats unregisters on destruction", "[telemetry]") {
  {
    Export fake(3, Export::Type::kFunction, "FakeC", ExportTag::kImplemented);
    ExportStats stats("xboxkrnl", &fake);
    stats.OnCall(0, [] { return 0u; });
    CHECK(ExportCallsJson().find("xboxkrnl::FakeC") != std::string::npos);
  }
  CHECK(ExportCallsJson().find("FakeC") == std::string::npos);
}

TEST_CASE("OnCall evaluates the thread id only on the first call",
          "[telemetry]") {
  Export fake(4, Export::Type::kFunction, "FakeD", ExportTag::kImplemented);
  ExportStats stats("xam", &fake);
  int evaluations = 0;
  for (int i = 0; i < 5; ++i) {
    stats.OnCall(0, [&] {
      ++evaluations;
      return 0u;
    });
  }
  CHECK(evaluations == 1);
  CHECK(stats.calls() == 5);
}

}  // namespace xe::kernel::util
