/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>

#include "third_party/catch/include/catch.hpp"

#include "xenia/base/platform.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"

namespace xe {
namespace cpu {
namespace test {

TEST_CASE("Device writes break reservations in every view", "[reservation]") {
  auto backend = std::make_unique<backend::x64::X64Backend>();
  // Physical 0x10000 through each physical view.
  uint32_t a = backend->reservation_generation(0xA0010000);
  uint32_t c = backend->reservation_generation(0xC0010000);
  uint32_t e = backend->reservation_generation(0xE000F000);
  // The next granule isn't written.
  uint32_t next = backend->reservation_generation(0xA0010080);
  backend->InvalidateReservations(0x10000, 4);
  REQUIRE(backend->reservation_generation(0xA0010000) != a);
  REQUIRE(backend->reservation_generation(0xC0010000) != c);
  REQUIRE(backend->reservation_generation(0xE000F000) != e);
  REQUIRE(backend->reservation_generation(0xA0010080) == next);
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
#endif  // XE_ARCH_AMD64
