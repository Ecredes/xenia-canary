/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/windowed_host.h"

#include <utility>

#include "xenia/base/logging.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_guest_notification.h"
#include "xenia/ui/imgui_host_notification.h"

namespace xe {
namespace app {

namespace {

// The window class's own default position unless the notification names one.
template <typename NotificationWindow>
void ShowNotification(ui::ImGuiDrawer* imgui_drawer,
                      const Notification& notification) {
  if (notification.position_id) {
    new NotificationWindow(imgui_drawer, notification.title,
                           notification.description, notification.user_index,
                           *notification.position_id);
  } else {
    new NotificationWindow(imgui_drawer, notification.title,
                           notification.description, notification.user_index);
  }
}

}  // namespace

void WindowedHost::Notify(Notification notification) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    XELOGI("Notification before the window exists: {} {}", notification.title,
           notification.description);
    return;
  }
  // Notification windows add themselves to the drawer, a UI thread object.
  app_context_.CallInUIThread(
      [imgui_drawer, notification = std::move(notification)]() {
        switch (notification.kind) {
          case Notification::Kind::kHost:
            ShowNotification<ui::HostNotificationWindow>(imgui_drawer,
                                                         notification);
            break;
          case Notification::Kind::kAchievement:
            ShowNotification<ui::AchievementNotificationWindow>(imgui_drawer,
                                                                notification);
            break;
          case Notification::Kind::kGuest:
            ShowNotification<ui::XNotifyWindow>(imgui_drawer, notification);
            break;
        }
      });
}

void WindowedHost::SetTitleIcon(std::vector<uint8_t> icon) {
  ui::Window* window = window_.load(std::memory_order_acquire);
  if (!window) {
    return;
  }
  app_context_.CallInUIThread([window, icon = std::move(icon)]() {
    window->SetIcon(icon.empty() ? nullptr : icon.data(), icon.size());
  });
}

void WindowedHost::ReportError(ErrorReport report) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    XELOGE("{}: {}", report.title, report.message);
    return;
  }
  app_context_.CallInUIThread([imgui_drawer, report = std::move(report)]() {
    ui::ImGuiDialog::ShowMessageBox(imgui_drawer, report.title, report.message);
  });
}

void WindowedHost::RequestDiscImage(
    std::string prompt, std::function<void(std::filesystem::path)> done) {
  // GTK only allows widgets to be built on the UI thread, and XamSwapDisc asks
  // from a guest thread.
  bool queued = app_context_.CallInUIThread([prompt, done]() {
    auto file_picker = ui::FilePicker::Create();
    file_picker->set_mode(ui::FilePicker::Mode::kOpen);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(false);
    file_picker->set_title(!prompt.empty() ? prompt : "Select Content Package");
    file_picker->set_extensions({
        {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
        {"Disc Image (*.iso)", "*.iso"},
        {"Xbox Executable (*.xex)", "*.xex"},
        {"All Files (*.*)", "*.*"},
    });
    std::filesystem::path path;
    if (file_picker->Show()) {
      auto selected_files = file_picker->selected_files();
      if (!selected_files.empty()) {
        path = selected_files[0];
      }
    }
    done(std::move(path));
  });
  if (!queued) {
    done({});
  }
}

}  // namespace app
}  // namespace xe
