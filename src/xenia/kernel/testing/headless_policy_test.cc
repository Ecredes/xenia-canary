/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#include "third_party/catch/include/catch.hpp"

#include "xenia/headless_policy.h"

namespace xe {
namespace test {

TEST_CASE("Headless exit code for a title ending itself", "[headless]") {
  // Runners restart on 75; a title back at the dashboard is a normal exit.
  REQUIRE(HeadlessExitCode({LifecycleRequest::Kind::kRelaunch}) == 75);
  REQUIRE(HeadlessExitCode({LifecycleRequest::Kind::kLaunchContent}) == 75);
  REQUIRE(HeadlessExitCode({LifecycleRequest::Kind::kTerminate}) ==
          EXIT_SUCCESS);
}

}  // namespace test
}  // namespace xe
