/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/headless_host.h"

#include <cstdlib>

#include "xenia/base/logging.h"
#include "xenia/headless_policy.h"

namespace xe {

void HeadlessHost::Notify(Notification notification) {
  XELOGI("Notification (user {}): {}{}{}", notification.user_index,
         notification.title, notification.title.empty() ? "" : ": ",
         notification.description);
}

void HeadlessHost::ReportError(ErrorReport report) {
  XELOGE("{}: {}", report.title, report.message);
}

void HeadlessHost::RequestDiscImage(
    std::string prompt, std::function<void(std::filesystem::path)> done) {
  XELOGW("No disc image to swap in without a UI: {}", prompt);
  done({});
}

void HeadlessHost::RequestChoice(ChoiceRequest request,
                                 std::function<void(uint32_t button)> done) {
  uint32_t button = HeadlessChoice(request);
  XELOGI("{}: answered \"{}\" without a UI", request.title,
         request.buttons.empty() ? "" : request.buttons[button]);
  done(button);
}

void HeadlessHost::RequestText(
    TextRequest request,
    std::function<void(std::optional<std::string> text)> done) {
  std::optional<std::string> text = HeadlessText(request);
  XELOGI("{}: entered \"{}\" without a UI", request.title, text.value_or(""));
  done(std::move(text));
}

void HeadlessHost::RequestSignIn(SignInRequest request,
                                 std::function<void(SignInAnswer)> done) {
  std::map<uint8_t, uint64_t> chosen = HeadlessSignIn(request);
  XELOGI("Sign-in without a UI: {} of {} profiles", chosen.size(),
         request.users_needed);
  done(std::move(chosen));
}

void HeadlessHost::RequestPasscode(PasscodeRequest request,
                                   std::function<void(PasscodeAnswer)> done) {
  XELOGI("{}: pass code entered without a UI", request.title);
  done(HeadlessPasscode(request));
}

void HeadlessHost::RequestProfileCreation(std::function<void()> done) {
  XELOGI("No profile created without a UI");
  done();
}

void HeadlessHost::ShowAchievements(uint64_t xuid, uint32_t title_id,
                                    std::function<void()> done) {
  XELOGI("Achievements of {:016X} in {:08X} not shown without a UI", xuid,
         title_id);
  done();
}

void HeadlessHost::ShowGamercard(uint64_t xuid, std::function<void()> done) {
  XELOGI("Gamercard of {:016X} not shown without a UI", xuid);
  done();
}

void HeadlessHost::RequestLifecycle(LifecycleRequest request) {
  int exit_code = HeadlessExitCode(request);
  XELOGI("The title ended itself, exiting with {}", exit_code);
  xe::FlushLog();
  std::quick_exit(exit_code);
}

}  // namespace xe
