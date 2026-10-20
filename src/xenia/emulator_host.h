/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_EMULATOR_HOST_H_
#define XENIA_EMULATOR_HOST_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xe {

// A notification for the user that needs no answer.
struct Notification {
  enum class Kind {
    // From the emulator itself, e.g. a skipped title update.
    kHost,
    kAchievement,
    // XNotifyQueueUI from the title.
    kGuest,
  };
  Kind kind;
  std::string title;
  std::string description;
  uint8_t user_index = 0;
  // Screen position id; unset for the kind's default.
  std::optional<uint8_t> position_id;
};

// An error the user should see, e.g. the title crashing.
struct ErrorReport {
  std::string title;
  std::string message;
};

// A choice of one of several buttons, e.g. a title's message box.
struct ChoiceRequest {
  std::string title;
  std::string text;
  // At least one.
  std::vector<std::string> buttons;
  // The answer if the dialog closes without a choice.
  uint32_t default_button = 0;
  // The answer when no one is there to ask. Not always the default: a "buy
  // the full game?" offer is answered "No" unattended.
  uint32_t unattended_button = 0;
};

// Text entry, e.g. a title's on-screen keyboard.
struct TextRequest {
  std::string title;
  std::string description;
  std::string default_text;
  // In characters, including the terminator.
  size_t max_length = 0;
};

// Signing users in to profiles, e.g. a title's sign-in prompt.
struct SignInRequest {
  uint32_t users_needed = 1;
  // The controller slot preselected when one user is needed.
  uint8_t last_used_slot = 0;
  // Profiles assigned to slots now: slot to XUID.
  std::map<uint8_t, uint64_t> signed_in;
};
// Profiles to sign in, slot to XUID, or none if cancelled.
using SignInAnswer = std::optional<std::map<uint8_t, uint64_t>>;

// A profile's pass code, asked by a title's pass code message box.
struct PasscodeRequest {
  std::string title;
  std::string description;
  // In keys.
  size_t length = 0;
};
// The pass code's keys, as XAM's pass code key values, or none if cancelled.
using PasscodeAnswer = std::optional<std::vector<uint16_t>>;

// The title ending itself; the host decides how the process goes on.
struct LifecycleRequest {
  enum class Kind {
    // XamLoaderLaunchTitle with a path: run again with the saved launch data.
    kRelaunch,
    // Back to the dashboard: XamLoaderTerminateTitle, or XamLoaderLaunchTitle
    // without a path.
    kTerminate,
    // A content package launched as a new title, from the saved launch data.
    kLaunchContent,
  };
  Kind kind;
};

// What the emulator needs from whatever runs it: the windowed UI, a runner
// without a window, or a test. The emulator speaks to it in plain data and
// never touches windows or UI itself. All methods may be called from any
// thread and never block on the user.
class EmulatorHost {
 public:
  virtual ~EmulatorHost() = default;

  virtual void Notify(Notification notification) = 0;
  // The running title's icon (an image file, as the title provides it), or
  // none when empty.
  virtual void SetTitleIcon(std::vector<uint8_t> icon) = 0;
  virtual void ReportError(ErrorReport report) = 0;
  // Asks for a disc image to swap in. done receives the chosen path, or an
  // empty one for none, on any thread.
  virtual void RequestDiscImage(
      std::string prompt, std::function<void(std::filesystem::path)> done) = 0;
  // done receives the chosen button's index, on any thread, exactly once.
  virtual void RequestChoice(ChoiceRequest request,
                             std::function<void(uint32_t button)> done) = 0;
  // done receives the entered text, or none if cancelled, on any thread,
  // exactly once.
  virtual void RequestText(
      TextRequest request,
      std::function<void(std::optional<std::string> text)> done) = 0;
  // done receives the profiles to sign in, on any thread, exactly once.
  virtual void RequestSignIn(SignInRequest request,
                             std::function<void(SignInAnswer)> done) = 0;
  virtual void RequestPasscode(PasscodeRequest request,
                               std::function<void(PasscodeAnswer)> done) = 0;
  // Lets the user create a profile; creating it is the host's own business.
  // done is called once the user is done, on any thread.
  virtual void RequestProfileCreation(std::function<void()> done) = 0;
  // The title then terminates; the host may end the process.
  virtual void RequestLifecycle(LifecycleRequest request) = 0;
};

}  // namespace xe

#endif  // XENIA_EMULATOR_HOST_H_
