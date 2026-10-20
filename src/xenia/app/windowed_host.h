/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_WINDOWED_HOST_H_
#define XENIA_APP_WINDOWED_HOST_H_

#include <atomic>

#include "xenia/emulator_host.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace app {

// Hosts the emulator in the main window: what the emulator asks for is shown
// with the ImGui UI, built on the UI thread.
class WindowedHost : public EmulatorHost {
 public:
  explicit WindowedHost(ui::WindowedAppContext& app_context)
      : app_context_(app_context) {}

  // The drawer exists only once the window does, after the emulator. Until
  // then, notifications go to the log.
  void SetImGuiDrawer(ui::ImGuiDrawer* imgui_drawer) {
    imgui_drawer_.store(imgui_drawer, std::memory_order_release);
  }

  void Notify(Notification notification) override;

 private:
  ui::WindowedAppContext& app_context_;
  std::atomic<ui::ImGuiDrawer*> imgui_drawer_ = nullptr;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_WINDOWED_HOST_H_
