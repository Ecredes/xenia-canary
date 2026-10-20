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
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace app {

// Hosts the emulator in the main window: what the emulator asks for is shown
// with the ImGui UI, built on the UI thread.
class WindowedHost : public EmulatorHost {
 public:
  explicit WindowedHost(ui::WindowedAppContext& app_context)
      : app_context_(app_context) {}

  // The window and its drawer exist only after the emulator. Until then,
  // notifications go to the log and the icon is dropped.
  void SetWindow(ui::Window* window, ui::ImGuiDrawer* imgui_drawer) {
    window_.store(window, std::memory_order_release);
    imgui_drawer_.store(imgui_drawer, std::memory_order_release);
  }

  void Notify(Notification notification) override;
  void SetTitleIcon(std::vector<uint8_t> icon) override;
  void ReportError(ErrorReport report) override;
  void RequestDiscImage(
      std::string prompt,
      std::function<void(std::filesystem::path)> done) override;
  void RequestChoice(ChoiceRequest request,
                     std::function<void(uint32_t button)> done) override;
  // Tells the user, then exits for a relaunch or termination once they close
  // the notice, saving the config.
  void RequestLifecycle(LifecycleRequest request) override;

 private:
  ui::WindowedAppContext& app_context_;
  std::atomic<ui::Window*> window_ = nullptr;
  std::atomic<ui::ImGuiDrawer*> imgui_drawer_ = nullptr;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_WINDOWED_HOST_H_
