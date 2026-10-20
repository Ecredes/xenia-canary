/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HEADLESS_POLICY_H_
#define XENIA_HEADLESS_POLICY_H_

#include <cstdlib>

#include "xenia/emulator_host.h"

namespace xe {

// What a run without a UI decides, with no one to ask. Pure, so the decisions
// are testable on their own.

// Exit status when the title asks to run again (with saved launch data):
// sysexits' EX_TEMPFAIL, "try again", which runners treat as restart.
constexpr int kExitCodeRelaunch = 75;

// The button chosen with no one to ask.
inline uint32_t HeadlessChoice(const ChoiceRequest& request) {
  return request.unattended_button < request.buttons.size()
             ? request.unattended_button
             : 0;
}

// The process exit status when the title ends itself.
inline int HeadlessExitCode(const LifecycleRequest& request) {
  switch (request.kind) {
    case LifecycleRequest::Kind::kRelaunch:
    case LifecycleRequest::Kind::kLaunchContent:
      return kExitCodeRelaunch;
    case LifecycleRequest::Kind::kTerminate:
      return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}

}  // namespace xe

#endif  // XENIA_HEADLESS_POLICY_H_
