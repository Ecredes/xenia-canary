/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/headless_host.h"

#include "xenia/base/logging.h"

namespace xe {

void HeadlessHost::Notify(Notification notification) {
  XELOGI("Notification (user {}): {}{}{}", notification.user_index,
         notification.title, notification.title.empty() ? "" : ": ",
         notification.description);
}

}  // namespace xe
