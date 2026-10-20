/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_FIRST_RUN_DISCLAIMER_H_
#define XENIA_APP_FIRST_RUN_DISCLAIMER_H_

namespace xe {
namespace app {

// Shows the piracy disclaimer, linking to the quickstart guide, the first time
// the user ever opens the windowed app. Remembered per user (Windows only).
void ShowFirstRunDisclaimer();

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_FIRST_RUN_DISCLAIMER_H_
