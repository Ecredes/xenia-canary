/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/system.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace xe {

namespace {
std::atomic<bool> interactive = true;
}  // namespace

void SetInteractive(bool is_interactive) {
  interactive.store(is_interactive, std::memory_order_relaxed);
#if XE_PLATFORM_WIN32
  if (!is_interactive) {
    // The CRT's assert and abort() report to stderr instead of a dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
  }
#endif
}

bool IsInteractive() { return interactive.load(std::memory_order_relaxed); }

void ShowSimpleMessageBox(SimpleMessageBoxType type,
                          const std::string_view message) {
  if (IsInteractive()) {
    ShowPlatformSimpleMessageBox(type, message);
    return;
  }
  // Help is the requested output; anything else is a diagnostic.
  std::FILE* stream = type == SimpleMessageBoxType::Help ? stdout : stderr;
  std::fwrite(message.data(), 1, message.size(), stream);
  std::fputc('\n', stream);
  std::fflush(stream);
}

}  // namespace xe
