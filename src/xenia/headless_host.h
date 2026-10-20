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
// dump, and later headless runs. What a user would see goes to the log.
class HeadlessHost : public EmulatorHost {
 public:
  void Notify(Notification notification) override;
};

}  // namespace xe

#endif  // XENIA_HEADLESS_HOST_H_
