/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/windowed_host.h"

#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

#include "xenia/app/choice_dialog.h"
#include "xenia/app/text_input_dialog.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xam/ui/passcode_ui.h"
#include "xenia/kernel/xam/ui/signin_ui.h"
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

// What the user is told when the title ends itself, and whether the process
// then exits.
struct LifecycleNotice {
  const char* title;
  const char* message;
  bool exit_when_closed;
};

LifecycleNotice GetLifecycleNotice(LifecycleRequest::Kind kind) {
  switch (kind) {
    case LifecycleRequest::Kind::kRelaunch:
      return {"Title was restarted",
              "Title closed with new launch data. \nPlease restart Xenia. "
              "Game will be loaded automatically.",
              true};
    case LifecycleRequest::Kind::kTerminate:
      return {"Title terminated", "Game requested exit to dashboard.", true};
    case LifecycleRequest::Kind::kLaunchContent:
      return {"Launching new title!",
              "Launching new title. \nPlease close Xenia and launch it "
              "again. Game should load automatically.",
              false};
  }
  return {"Title terminated", "", true};
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

void WindowedHost::RequestChoice(ChoiceRequest request,
                                 std::function<void(uint32_t button)> done) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    done(request.default_button);
    return;
  }
  uint32_t default_button = request.default_button;
  // The dialog answers when destroyed, also if it never opened.
  bool queued = app_context_.CallInUIThread(
      [imgui_drawer, request = std::move(request), done]() mutable {
        new ChoiceDialog(imgui_drawer, std::move(request), std::move(done));
      });
  if (!queued) {
    done(default_button);
  }
}

void WindowedHost::RequestText(
    TextRequest request,
    std::function<void(std::optional<std::string> text)> done) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    done(std::nullopt);
    return;
  }
  // The dialog answers when destroyed, also if it never opened.
  bool queued = app_context_.CallInUIThread(
      [imgui_drawer, request = std::move(request), done]() mutable {
        new TextInputDialog(imgui_drawer, std::move(request), std::move(done));
      });
  if (!queued) {
    done(std::nullopt);
  }
}

void WindowedHost::RequestSignIn(SignInRequest request,
                                 std::function<void(SignInAnswer)> done) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    done(std::nullopt);
    return;
  }
  bool queued = app_context_.CallInUIThread([imgui_drawer, request, done]() {
    new kernel::xam::ui::SigninUI(imgui_drawer, request, done);
  });
  if (!queued) {
    done(std::nullopt);
  }
}

void WindowedHost::RequestPasscode(PasscodeRequest request,
                                   std::function<void(PasscodeAnswer)> done) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    done(std::nullopt);
    return;
  }
  bool queued = app_context_.CallInUIThread([imgui_drawer, request, done]() {
    new kernel::xam::ui::ProfilePasscodeUI(imgui_drawer, request, done);
  });
  if (!queued) {
    done(std::nullopt);
  }
}

void WindowedHost::RequestProfileCreation(std::function<void()> done) {
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    done();
    return;
  }
  bool queued = app_context_.CallInUIThread([imgui_drawer, done]() {
    auto dialog = new kernel::xam::ui::CreateProfileUI(
        imgui_drawer, kernel::kernel_state()->emulator());
    dialog->set_close_callback(done);
  });
  if (!queued) {
    done();
  }
}

void WindowedHost::RequestLifecycle(LifecycleRequest request) {
  LifecycleNotice notice = GetLifecycleNotice(request.kind);
  ui::ImGuiDrawer* imgui_drawer = imgui_drawer_.load(std::memory_order_acquire);
  if (!imgui_drawer) {
    XELOGI("{}: {}", notice.title, notice.message);
    return;
  }
  app_context_.CallInUIThread([imgui_drawer, notice]() {
    ui::ImGuiDialog* dialog = ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer, notice.title, notice.message);
    if (!notice.exit_when_closed) {
      return;
    }
    // Signaled when the dialog is destroyed after closing.
    auto closed = std::make_shared<xe::threading::Fence>();
    dialog->Then(closed.get());
    std::thread([closed]() {
      closed->Wait();
      config::SaveConfig();
      xe::FlushLog();
      std::quick_exit(EXIT_SUCCESS);
    }).detach();
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
