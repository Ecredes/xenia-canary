/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <fstream>

#include "third_party/catch/include/catch.hpp"

#include "xenia/kernel/xam/launch_data.h"

namespace xe {
namespace kernel {
namespace xam {
namespace test {

TEST_CASE("Launch data applies only to its own title", "[launch_data]") {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "xenia-launch-data-test";
  std::filesystem::create_directories(dir);
  std::filesystem::path title_a = dir / "a.iso";
  std::filesystem::path title_b = dir / "b.iso";
  std::ofstream(title_a).put('a');
  std::ofstream(title_b).put('b');

  SECTION("no saved data") {
    REQUIRE_FALSE(LaunchDataAppliesTo({}, title_a));
    REQUIRE_FALSE(LaunchDataAppliesTo({}, {}));
  }
  SECTION("restart without a target resumes the saved title") {
    REQUIRE(LaunchDataAppliesTo(title_a, {}));
  }
  SECTION("restart with the same title") {
    REQUIRE(LaunchDataAppliesTo(title_a, title_a));
    REQUIRE(LaunchDataAppliesTo(title_a, dir / "." / "a.iso"));
  }
  SECTION("another title is not affected") {
    REQUIRE_FALSE(LaunchDataAppliesTo(title_a, title_b));
  }
  SECTION("a saved title that no longer exists") {
    REQUIRE(LaunchDataAppliesTo(dir / "gone.iso", dir / "gone.iso"));
    REQUIRE_FALSE(LaunchDataAppliesTo(dir / "gone.iso", title_b));
  }

  std::filesystem::remove_all(dir);
}

}  // namespace test
}  // namespace xam
}  // namespace kernel
}  // namespace xe
