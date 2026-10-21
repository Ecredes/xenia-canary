/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_ANSWER_SOURCE_H_
#define XENIA_ANSWER_SOURCE_H_

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <variant>

#include "xenia/emulator_host.h"
#include "xenia/headless_policy.h"

namespace xe {

// A disc image for the title to swap in.
struct DiscRequest {
  std::string prompt;
};
// The image's host path, empty for none.
using DiscAnswer = std::filesystem::path;

// Every prompt HeadlessHost answers, and its answer at the same index.
using DialogRequest = std::variant<ChoiceRequest, TextRequest, SignInRequest,
                                   PasscodeRequest, DiscRequest>;
using DialogAnswer = std::variant<uint32_t, std::optional<std::string>,
                                  SignInAnswer, PasscodeAnswer, DiscAnswer>;

// The answer with no one to ask: the headless policies.
inline DialogAnswer PolicyAnswer(const DialogRequest& request) {
  switch (request.index()) {
    case 0:
      return DialogAnswer(std::in_place_index<0>,
                          HeadlessChoice(std::get<0>(request)));
    case 1:
      return DialogAnswer(std::in_place_index<1>,
                          HeadlessText(std::get<1>(request)));
    case 2:
      return DialogAnswer(std::in_place_index<2>,
                          HeadlessSignIn(std::get<2>(request)));
    case 3:
      return DialogAnswer(std::in_place_index<3>,
                          HeadlessPasscode(std::get<3>(request)));
    default:
      return DialogAnswer(std::in_place_index<4>);
  }
}

// Where HeadlessHost gets answers. done gets an answer of the request's kind
// (the same variant index), exactly once, on any thread.
class AnswerSource {
 public:
  virtual ~AnswerSource() = default;
  virtual void Answer(DialogRequest request,
                      std::function<void(DialogAnswer)> done) = 0;
};

class PolicyAnswerSource : public AnswerSource {
 public:
  void Answer(DialogRequest request,
              std::function<void(DialogAnswer)> done) override {
    done(PolicyAnswer(request));
  }
};

}  // namespace xe

#endif  // XENIA_ANSWER_SOURCE_H_
