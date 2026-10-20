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

TEST_CASE("Headless choice answers the unattended button", "[headless]") {
  ChoiceRequest offer{"Xbox Marketplace", "", {"Yes", "No"}, 0, 1};
  REQUIRE(HeadlessChoice(offer) == 1);
  ChoiceRequest message_box{"", "", {"A", "B", "C"}, 2, 2};
  REQUIRE(HeadlessChoice(message_box) == 2);
  // A title's out-of-range default falls back to the first button.
  ChoiceRequest bad_default{"", "", {"OK"}, 3, 3};
  REQUIRE(HeadlessChoice(bad_default) == 0);
}

TEST_CASE("Headless text entry keeps the title's default", "[headless]") {
  TextRequest name{"Name", "Enter your name", "Player", 16};
  REQUIRE(HeadlessText(name) == std::optional<std::string>("Player"));
  // Not cancelled even without a default.
  REQUIRE(HeadlessText(TextRequest{}) == std::optional<std::string>(""));
}

TEST_CASE("Headless sign-in keeps the profiles in slots", "[headless]") {
  SignInRequest one{1, 0, {{1, 0xE001}, {3, 0xE003}}};
  REQUIRE(HeadlessSignIn(one) == std::map<uint8_t, uint64_t>{{1, 0xE001}});
  SignInRequest two{2, 0, {{1, 0xE001}, {3, 0xE003}}};
  REQUIRE(HeadlessSignIn(two) ==
          std::map<uint8_t, uint64_t>{{1, 0xE001}, {3, 0xE003}});
  // No profile in a slot: no one signs in.
  REQUIRE(HeadlessSignIn(SignInRequest{1, 0, {}}).empty());
}

TEST_CASE("Headless pass code is every key none", "[headless]") {
  REQUIRE(HeadlessPasscode({"", "", 4}) == std::vector<uint16_t>(4, 0));
}

}  // namespace test
}  // namespace xe
