/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/config.h"

#include "third_party/catch/include/catch.hpp"

DEFINE_int32(config_test_layer_value, 1, "Test-only config variable.",
             "ConfigTest");

namespace xe::base::test {

namespace {

class ScratchConfigFolder {
 public:
  ScratchConfigFolder() {
    std::random_device rng;
    path_ = std::filesystem::temp_directory_path() /
            ("xenia_config_test_" + std::to_string(rng()) + "_" +
             std::to_string(rng()));
    std::filesystem::create_directories(path_ / "config");
  }
  ~ScratchConfigFolder() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

  void Write(const std::filesystem::path& relative,
             const std::string& contents) const {
    std::ofstream(path_ / relative, std::ios::binary) << contents;
  }

  int64_t SavedValue() const {
    auto table =
        toml::parse_file(xe::path_to_utf8(path_ / "xenia-canary.config.toml"));
    return table["ConfigTest"]["config_test_layer_value"].value_or<int64_t>(-1);
  }

 private:
  std::filesystem::path path_;
};

std::string ConfigFile(int32_t value) {
  // A current defaults_date keeps ReadConfig from applying default updates.
  return fmt::format(
      "[Config]\ndefaults_date = {}\n\n[ConfigTest]\n"
      "config_test_layer_value = {}\n",
      cvar::IConfigVarUpdate::GetLastUpdateDate(), value);
}

}  // namespace

TEST_CASE("Per-title config values stay out of the global config", "[config]") {
  ScratchConfigFolder folder;
  folder.Write("xenia-canary.config.toml", ConfigFile(2));
  folder.Write("config/58410001.config.toml",
               "[ConfigTest]\nconfig_test_layer_value = 3\n");

  config::SetupConfig(folder.path());
  REQUIRE(cvars::config_test_layer_value == 2);

  config::LoadGameConfig("58410001");
  CHECK(cvars::config_test_layer_value == 3);

  config::SaveConfig();
  CHECK(folder.SavedValue() == 2);

  SECTION("Clearing the per-title layer restores the global value") {
    config::ClearGameConfig();
    CHECK(cvars::config_test_layer_value == 2);
  }

  SECTION("The next title doesn't inherit the previous title's values") {
    config::LoadGameConfig("58410002");
    CHECK(cvars::config_test_layer_value == 2);
  }

  config::ClearGameConfig();
}

}  // namespace xe::base::test
