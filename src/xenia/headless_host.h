/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HEADLESS_HOST_H_
#define XENIA_HEADLESS_HOST_H_

#include "xenia/emulator_host.h"

namespace xe {

// Hosts the emulator with no one to show anything to: tools like the GPU trace
// dump, and headless runs. What a user would see goes to the log.
class HeadlessHost : public EmulatorHost {
 public:
  void Notify(Notification notification) override;
  // Nothing shows it.
  void SetTitleIcon(std::vector<uint8_t> icon) override {}
  void ReportError(ErrorReport report) override;
  // No one to choose one: no disc.
  void RequestDiscImage(
      std::string prompt,
      std::function<void(std::filesystem::path)> done) override;
  // Answers with HeadlessChoice.
  void RequestChoice(ChoiceRequest request,
                     std::function<void(uint32_t button)> done) override;
  // Answers with HeadlessText.
  void RequestText(
      TextRequest request,
      std::function<void(std::optional<std::string> text)> done) override;
  // Answers with HeadlessSignIn, HeadlessPasscode, and creates no profile.
  // Shows nothing.
  void RequestSignIn(SignInRequest request,
                     std::function<void(SignInAnswer)> done) override;
  void RequestPasscode(PasscodeRequest request,
                       std::function<void(PasscodeAnswer)> done) override;
  void RequestProfileCreation(std::function<void()> done) override;
  void ShowAchievements(uint64_t xuid, uint32_t title_id,
                        std::function<void()> done) override;
  void ShowGamercard(uint64_t xuid, std::function<void()> done) override;
  // Ends the process with HeadlessExitCode.
  void RequestLifecycle(LifecycleRequest request) override;
};

}  // namespace xe

#endif  // XENIA_HEADLESS_HOST_H_
