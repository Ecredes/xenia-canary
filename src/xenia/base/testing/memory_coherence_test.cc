/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/memory_coherence.h"

namespace xe {
namespace test {

using namespace xe::coherence;

TEST_CASE("Coherence: a GPU write owns its pages", "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x2000, 1, 0, 5});
  REQUIRE(core.IsGpuOwned(0x10));
  REQUIRE(core.IsGpuOwned(0x11));
  REQUIRE_FALSE(core.IsGpuOwned(0x12));
  REQUIRE(core.FenceFor(0x10000, 4) == 5);
}

TEST_CASE("Coherence: a pull takes only the touched page, once",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x2000, 1, 0, 5});
  REQUIRE(core.TakeCopies(0x10010, 4) ==
          std::vector<Copy>{{1, 0, 0x10000, 0x1000, 5}});
  REQUIRE(core.TakeCopies(0x10010, 4).empty());
  REQUIRE_FALSE(core.IsGpuOwned(0x10));
  REQUIRE(core.IsGpuOwned(0x11));
  REQUIRE(core.stats().pages_pulled == 1);
}

TEST_CASE("Coherence: a newer write supersedes overlapped bytes",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x1000, 1, 0, 5});
  core.RecordGpuWrite({0x10800, 0x1000, 2, 0, 6});
  REQUIRE(
      core.TakeCopies(0x10000, 0x2000) ==
      std::vector<Copy>{{1, 0, 0x10000, 0x800, 5}, {2, 0, 0x10800, 0x1000, 6}});
  REQUIRE(core.stats().bytes_superseded == 0x800);
}

TEST_CASE("Coherence: a partial write leaves the page's other bytes alone",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10100, 0x100, 1, 0x40, 5});
  REQUIRE(core.TakeCopies(0x10000, 0x1000) ==
          std::vector<Copy>{{1, 0x40, 0x10100, 0x100, 5}});
}

TEST_CASE("Coherence: buffer reuse takes its pending copies",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x1000, 1, 0, 5});
  core.RecordGpuWrite({0x20000, 0x1000, 2, 0, 6});
  REQUIRE(core.TakeCopiesFromBuffer(1) ==
          std::vector<Copy>{{1, 0, 0x10000, 0x1000, 5}});
  REQUIRE_FALSE(core.IsGpuOwned(0x10));
  REQUIRE(core.IsGpuOwned(0x20));
}

TEST_CASE("Coherence: a buffer's fence is its newest pending one",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x1000, 1, 0, 5});
  core.RecordGpuWrite({0x20000, 0x1000, 1, 0x1000, 7});
  core.RecordGpuWrite({0x30000, 0x1000, 2, 0, 9});
  REQUIRE(core.FenceForBuffer(1) == 7);
  REQUIRE(core.FenceForBuffer(3) == 0);
  core.TakeCopiesFromBuffer(1);
  REQUIRE(core.FenceForBuffer(1) == 0);
}

// A resolve whose readback is cut short records only its head; the tail keeps
// the older pending write, which is what eager readback leaves in guest RAM.
TEST_CASE("Coherence: an unrecorded tail keeps the older write",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x2000, 1, 0, 5});
  core.RecordGpuWrite({0x10000, 0x1000, 2, 0, 6});
  REQUIRE(core.TakeCopies(0x10000, 0x2000) ==
          std::vector<Copy>{{1, 0x1000, 0x11000, 0x1000, 5},
                            {2, 0, 0x10000, 0x1000, 6}});
}

TEST_CASE("Coherence: discard drops pending bytes", "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x2000, 1, 0, 5});
  core.Discard(0x10000, 0x1000);
  REQUIRE_FALSE(core.IsGpuOwned(0x10));
  REQUIRE(core.FenceFor(0x10000, 1) == 0);
  REQUIRE(core.IsGpuOwned(0x11));
  REQUIRE(core.stats().bytes_discarded == 0x1000);
}

TEST_CASE("Coherence: discard returns what it dropped", "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x2000, 1, 0, 5});
  REQUIRE(core.Discard(0x10800, 0x1000) ==
          std::vector<Copy>{{1, 0x800, 0x10800, 0x1000, 5}});
}

TEST_CASE("Coherence: the fence is the newest overlapping one",
          "[memory_coherence]") {
  Core core;
  core.RecordGpuWrite({0x10000, 0x100, 1, 0, 5});
  core.RecordGpuWrite({0x10800, 0x100, 2, 0, 9});
  REQUIRE(core.FenceFor(0x10000, 4) == 9);  // same page
  REQUIRE(core.FenceFor(0x11000, 4) == 0);
}

TEST_CASE("Coherence: host access per view", "[memory_coherence]") {
  using A = Access;
  REQUIRE(EffectiveHostAccess({A::kReadWrite, true, false}, true) ==
          A::kNoAccess);
  REQUIRE(EffectiveHostAccess({A::kNoAccess, false, false}, true) ==
          A::kNoAccess);
  REQUIRE(EffectiveHostAccess({A::kNoAccess, false, false}, false) ==
          A::kReadWrite);
  REQUIRE(EffectiveHostAccess({A::kNoAccess, false, true}, false) ==
          A::kReadOnly);
  REQUIRE(EffectiveHostAccess({A::kReadWrite, true, true}, false) ==
          A::kReadOnly);
  REQUIRE(EffectiveHostAccess({A::kReadWrite, true, false}, false) ==
          A::kReadWrite);
  REQUIRE(EffectiveHostAccess({A::kReadOnly, true, true}, false) ==
          A::kReadOnly);
  REQUIRE(EffectiveHostAccess({A::kNoAccess, true, false}, false) ==
          A::kNoAccess);
}

}  // namespace test
}  // namespace xe
