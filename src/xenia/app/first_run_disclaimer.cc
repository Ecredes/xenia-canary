/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/first_run_disclaimer.h"

#include <cstdint>

#include "xenia/base/platform.h"
#include "xenia/base/system.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace app {

#if XE_PLATFORM_WIN32
namespace {

enum : uint64_t { kFlagDisclaimerAcknowledged = 1ULL << 0 };

void SetPersistentFlags(uint64_t new_flags) {
  uint64_t value = new_flags;
  HKEY xenia_hkey = nullptr;
  RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  }

  RegSetValueExA(xenia_hkey, "XEFLAGS", 0, REG_QWORD,
                 reinterpret_cast<const BYTE*>(&value), 8);
  RegFlushKey(xenia_hkey);
  RegCloseKey(xenia_hkey);
}

uint64_t GetPersistentFlags() {
  uint64_t value = 0;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    // let the Set function create the key and initialize it to 0
    SetPersistentFlags(0ULL);
    return 0ULL;
  }

  LSTATUS lstat =
      RegQueryValueExA(xenia_hkey, "XEFLAGS", 0, NULL,
                       reinterpret_cast<LPBYTE>(&value), &value_size);
  RegCloseKey(xenia_hkey);
  if (lstat) {
    return 0ULL;
  }
  return value;
}

}  // namespace
#endif  // XE_PLATFORM_WIN32

void ShowFirstRunDisclaimer() {
#if XE_PLATFORM_WIN32
  uint64_t persistent_flags = GetPersistentFlags();
  if (persistent_flags & kFlagDisclaimerAcknowledged) {
    return;
  }
  if ((MessageBoxW(
           nullptr,
           L"DISCLAIMER: Xenia is not for enabling illegal activity, and "
           "support is unavailable for illegally obtained software.\n\n"
           "Please respect this policy as no further reminders will be "
           "given.\n\nThe quickstart guide explains how to use digital or "
           "physical games from your Xbox 360 console.\n\nWould you like "
           "to open it?",
           L"Xenia", MB_YESNO | MB_ICONQUESTION) == IDYES)) {
    LaunchWebBrowser(
        "https://github.com/xenia-canary/xenia-canary/wiki/"
        "Quickstart#how-to-rip-games");
  }
  SetPersistentFlags(persistent_flags | kFlagDisclaimerAcknowledged);
#endif
}

}  // namespace app
}  // namespace xe
