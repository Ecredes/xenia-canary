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

#include <atomic>

#include "xenia/answer_source.h"
#include "xenia/emulator_host.h"

namespace xe {

// Hosts the emulator with no one to show anything to: tools like the GPU trace
// dump, and headless runs. What a user would see goes to the log.
class HeadlessHost : public EmulatorHost {
 public:
  // Where prompts are answered from, the policies when null. The source must
  // outlive any prompt it is asked.
  void set_answer_source(AnswerSource* source) {
    answer_source_.store(source, std::memory_order_release);
  }

  void Notify(Notification notification) override;
  // Nothing shows it.
  void SetTitleIcon(std::vector<uint8_t> icon) override {}
  void ReportError(ErrorReport report) override;
  // The prompts below are answered by the answer source.
  void RequestDiscImage(
      std::string prompt,
      std::function<void(std::filesystem::path)> done) override;
  void RequestChoice(ChoiceRequest request,
                     std::function<void(uint32_t button)> done) override;
  void RequestText(
      TextRequest request,
      std::function<void(std::optional<std::string> text)> done) override;
  void RequestSignIn(SignInRequest request,
                     std::function<void(SignInAnswer)> done) override;
  void RequestPasscode(PasscodeRequest request,
                       std::function<void(PasscodeAnswer)> done) override;
  // Creates no profile and shows nothing.
  void RequestProfileCreation(std::function<void()> done) override;
  void ShowAchievements(uint64_t xuid, uint32_t title_id,
                        std::function<void()> done) override;
  void ShowGamercard(uint64_t xuid, std::function<void()> done) override;
  // Ends the process with HeadlessExitCode.
  void RequestLifecycle(LifecycleRequest request) override;

 private:
  AnswerSource& answer_source() {
    AnswerSource* source = answer_source_.load(std::memory_order_acquire);
    return source ? *source : policy_;
  }

  PolicyAnswerSource policy_;
  std::atomic<AnswerSource*> answer_source_ = nullptr;
};

}  // namespace xe

#endif  // XENIA_HEADLESS_HOST_H_
