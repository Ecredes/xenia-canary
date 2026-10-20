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
};

}  // namespace xe

#endif  // XENIA_EMULATOR_HOST_H_
