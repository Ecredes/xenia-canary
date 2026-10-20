/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui.h"

#include <cstring>

#include "xenia/base/system.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

DEFINE_bool(storage_selection_dialog, false,
            "Show storage device selection dialog when the game requests it.",
            "UI");

DECLARE_int32(license_mask);

constexpr std::chrono::milliseconds kUIDelayMillis(200);

namespace xe {
namespace kernel {
namespace xam {
// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).
// Asks the host and waits for its answer: a XAM dialog blocks the thread that
// shows it (the title's, or the kernel dispatch thread for an overlapped one).
template <typename Answer>
Answer xeXamWaitForAnswer(
    std::function<void(std::function<void(Answer)>)> ask) {
  Answer answer{};
  xe::threading::Fence answered;
  ask([&answer, &answered](Answer host_answer) {
    answer = std::move(host_answer);
    answered.Signal();
  });
  answered.Wait();
  return answer;
}

// The guest-visible side of a XAM dialog, whoever answers it through the host
// (a user, or a policy without a UI): one dialog at a time, the system UI
// notification around it, and completion like any XAM call, blocking or
// through an overlapped. ask passes the request to the host.
template <typename Answer>
X_RESULT xeXamShowDialog(std::function<void(std::function<void(Answer)>)> ask,
                         std::function<X_RESULT(Answer)> on_answer,
                         uint32_t overlapped) {
  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }
  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [ask = std::move(ask), on_answer]() -> X_RESULT {
    Answer answer = xeXamWaitForAnswer<Answer>(ask);
    kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    return on_answer(std::move(answer));
  };
  auto post = []() {
    std::jthread t([] {
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    });
    t.detach();
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  }
  kernel_state()->CompleteOverlappedDeferred(run, overlapped, pre, post);
  return X_ERROR_IO_PENDING;
}

X_RESULT xeXamShowChoice(ChoiceRequest request,
                         std::function<X_RESULT(uint32_t button)> on_answer,
                         uint32_t overlapped) {
  return xeXamShowDialog<uint32_t>(
      [request = std::move(request)](std::function<void(uint32_t)> done) {
        kernel_state()->emulator()->host().RequestChoice(request,
                                                         std::move(done));
      },
      std::move(on_answer), overlapped);
}

// The same for text entry, completed with an extended error and a length.
X_RESULT xeXamShowText(
    TextRequest request,
    std::function<X_RESULT(std::optional<std::string> text,
                           uint32_t& extended_error, uint32_t& length)>
        on_answer,
    uint32_t overlapped) {
  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }
  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  };
  auto run = [request = std::move(request), on_answer](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    std::optional<std::string> text =
        xeXamWaitForAnswer<std::optional<std::string>>(
            [&request](std::function<void(std::optional<std::string>)> done) {
              kernel_state()->emulator()->host().RequestText(request,
                                                             std::move(done));
            });
    kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    return on_answer(std::move(text), extended_error, length);
  };
  auto post = []() {
    xe::threading::Sleep(kUIDelayMillis);
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    return result;
  }
  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
  return X_ERROR_IO_PENDING;
}

// The same for a dialog the title doesn't wait for: on_answer runs later.
template <typename Answer>
X_RESULT xeXamShowDialogAsync(
    std::function<void(std::function<void(Answer)>)> ask,
    std::function<void(Answer)> on_answer) {
  if (kernel_state()->xam_state()->IsUIActive()) {
    return X_ERROR_ACCESS_DENIED;
  }
  kernel_state()->xam_state()->is_xam_dialog_present_.store(true);
  kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  ask([on_answer](Answer answer) {
    on_answer(std::move(answer));
    kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    std::thread([] {
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    }).detach();
  });
  return X_ERROR_SUCCESS;
}

X_RESULT xeXamShowChoiceAsync(ChoiceRequest request,
                              std::function<void(uint32_t button)> on_answer) {
  return xeXamShowDialogAsync<uint32_t>(
      [request = std::move(request)](std::function<void(uint32_t)> done) {
        kernel_state()->emulator()->host().RequestChoice(request,
                                                         std::move(done));
      },
      std::move(on_answer));
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback,
                               uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
    xe::threading::Sleep(std::chrono::milliseconds(25));
  };
  auto post = []() {
    std::jthread t([]() {
      xe::threading::Sleep(kUIDelayMillis);
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    });

    t.detach();
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    kernel_state()->CompleteOverlappedDeferred(run_callback, overlapped, pre,
                                               post);
    return X_ERROR_IO_PENDING;
  }
}

static dword_result_t XamShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  std::string title = title_ptr ? xe::to_utf8(title_ptr.value()) : "";
  std::string text = text_ptr ? xe::to_utf8(text_ptr.value()) : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptr));

    if (!button.empty()) {
      buttons.push_back(xe::to_utf8(button));
    }
  }

  if (buttons.empty()) {
    buttons.push_back("OK");
  }

  if (flags & XMBox_PASSCODEMODE || flags & XMBox_VERIFYPASSCODEMODE) {
    PasscodeRequest request{title, text,
                            sizeof(result_ptr->Passcode) / sizeof(uint16_t)};
    auto on_answer = [result_ptr](PasscodeAnswer keys) -> X_RESULT {
      std::memset(result_ptr, 0, sizeof(MESSAGEBOX_RESULT));
      if (!keys) {
        return X_ERROR_FUNCTION_FAILED;
      }
      for (size_t i = 0;
           i < keys->size() && i < xe::countof(result_ptr->Passcode); i++) {
        result_ptr->Passcode[i] = (*keys)[i];
      }
      return X_ERROR_SUCCESS;
    };
    return xeXamShowDialog<PasscodeAnswer>(
        [request =
             std::move(request)](std::function<void(PasscodeAnswer)> done) {
          kernel_state()->emulator()->host().RequestPasscode(request,
                                                             std::move(done));
        },
        on_answer, overlapped);
  }

  auto on_answer = [result_ptr](uint32_t button) -> X_RESULT {
    result_ptr->ButtonPressed = button;
    return X_ERROR_SUCCESS;
  };
  return xeXamShowChoice(
      {title, text, std::move(buttons), static_cast<uint32_t>(active_button),
       static_cast<uint32_t>(active_button)},
      on_answer, overlapped);
}

dword_result_t XamIsUIActive_entry() {
  return kernel_state()->xam_state()->IsUIActive();
}
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
dword_result_t XamShowMessageBoxUI_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

dword_result_t XamShowMessageBoxUIEx_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, dword_t unknown_unused,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags, result_ptr,
                             overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUIEx, kUI, kImplemented);

// XamShowMessageBoxUI with the tracking ID of a Kinect skeleton first.
// TODO(knuckleslee): Selecting the buttons with Kinect is not supported.
dword_result_t XamShowNuiMessageBoxUI_entry(
    dword_t tracking_id, dword_t user_index, lpu16string_t title_ptr,
    lpu16string_t text_ptr, dword_t button_count, lpdword_t button_ptrs,
    dword_t active_button, dword_t flags,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  return XamShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                             button_ptrs, active_button, flags | 0x2000,
                             result_ptr, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowNuiMessageBoxUI, kUI, kSketchy);

dword_result_t XNotifyQueueUI_entry(dword_t exnq, dword_t dwUserIndex,
                                    qword_t qwAreas,
                                    lpu16string_t displayText_ptr,
                                    lpvoid_t contextData) {
  std::string displayText = "";
  const uint8_t position_id = static_cast<uint8_t>(qwAreas);

  if (displayText_ptr) {
    displayText = xe::to_utf8(displayText_ptr.value());
  }

  XELOGI("XNotifyQueueUI: {}", displayText);

  kernel_state()->emulator()->host().Notify(
      {Notification::Kind::kGuest, "", displayText,
       static_cast<uint8_t>(dwUserIndex.value()), position_id});

  // XNotifyQueueUI -> XNotifyQueueUIEx -> XMsgProcessRequest ->
  // XMsgStartIORequestEx & XMsgInProcessCall
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyQueueUI, kUI, kSketchy);

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
dword_result_t XamShowKeyboardUI_entry(
    dword_t user_index, dword_t flags, lpu16string_t default_text,
    lpu16string_t title, lpu16string_t description, lpu16string_t buffer,
    dword_t buffer_length, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_not_null(overlapped);

  auto on_answer = [buffer, buffer_length](std::optional<std::string> text,
                                           uint32_t& extended_error,
                                           uint32_t& length) -> X_RESULT {
    length = 0;
    if (!text) {
      extended_error = X_ERROR_CANCELLED;
      return X_ERROR_SUCCESS;
    }
    string_util::copy_and_swap_truncating(buffer, xe::to_utf16(*text),
                                          buffer_length);
    extended_error = X_ERROR_SUCCESS;
    return X_ERROR_SUCCESS;
  };

  return xeXamShowText({title ? xe::to_utf8(title.value()) : "",
                        description ? xe::to_utf8(description.value()) : "",
                        default_text ? xe::to_utf8(default_text.value()) : "",
                        static_cast<size_t>(buffer_length)},
                       on_answer, overlapped);
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI_entry(
    dword_t user_index, dword_t content_type, dword_t content_flags,
    qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!overlapped) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if ((user_index >= XUserMaxUserCount && user_index != XUserIndexAny) ||
      (content_flags & 0x83F00008) != 0 || !device_id_ptr) {
    XOverlappedSetExtendedError(overlapped, X_ERROR_INVALID_PARAMETER);
    return X_ERROR_INVALID_PARAMETER;
  }

  std::vector<const DummyDeviceInfo*> devices = ListStorageDevices();

  if (!cvars::storage_selection_dialog) {
    // Default to the first storage device (HDD) without asking.
    return xeXamDispatchHeadless(
        [device_id_ptr, devices]() -> X_RESULT {
          if (devices.empty()) {
            return X_ERROR_CANCELLED;
          }

          const DummyDeviceInfo* device_info = devices.front();
          *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
          return X_ERROR_SUCCESS;
        },
        overlapped);
  }

  auto on_answer = [device_id_ptr, devices](uint32_t button) -> X_RESULT {
    if (button >= devices.size()) {
      return X_ERROR_CANCELLED;
    }

    const DummyDeviceInfo* device_info = devices.at(button);
    *device_id_ptr = static_cast<uint32_t>(device_info->device_id);
    return X_ERROR_SUCCESS;
  };

  std::vector<std::string> buttons;
  for (auto& device_info : devices) {
    buttons.push_back(to_utf8(device_info->name));
  }
  buttons.push_back("Cancel");

  return xeXamShowChoice(
      {"Select storage device", "", std::move(buttons), 0, 0}, on_answer,
      overlapped);
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

// XamShowDeviceSelectorUI with the tracking ID of a Kinect skeleton first.
dword_result_t XamShowNuiDeviceSelectorUI_entry(
    dword_t tracking_id, dword_t user_index, dword_t content_type,
    dword_t content_flags, qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  return XamShowDeviceSelectorUI_entry(user_index, content_type, content_flags,
                                       total_requested, device_id_ptr,
                                       overlapped);
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kUI, kSketchy);

std::string disc_title = "Disc Read Error";
std::string disc_desc =
    "There's been an issue reading content from the game disc.\nThis is "
    "likely caused by bad or unimplemented file IO calls.";

dword_result_t XamShowDirtyDiscErrorUI_entry(dword_t user_index) {
  // uses XamIsDataCenterMode & XamShowAndWaitForMessageBoxEx, same as
  // XamShowNuiDirtyDiscErrorUI except flag = 1, user index always set to
  // XUserIndexAny
  xeXamShowChoice(
      {disc_title, disc_desc, {"OK"}},
      [](uint32_t) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kStub);

dword_result_t XamShowNuiDirtyDiscErrorUI_entry(dword_t tracking_id) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  // XamShowAndWaitForMessageBoxEx, same as XamShowDirtyDiscErrorUI except flag
  // = 0x2001, user index always set to XUserIndexAny
  xeXamShowChoice(
      {disc_title, disc_desc, {"OK"}},
      [](uint32_t) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDirtyDiscErrorUI, kUI, kStub);

dword_result_t XamShowPartyUI_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowPartyUI, kUI, kStub);

dword_result_t XamShowCommunitySessionsUI_entry(dword_t user_index,
                                                dword_t social_sessions_flags) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowCommunitySessionsUI, kUI, kStub);

// https://gitlab.com/GlitchyScripts/xlivelessness/-/blob/master/xlivelessness/xlive/xdefs.hpp?ref_type=heads#L1235
dword_result_t XamShowMarketplaceUIEx_entry(dword_t user_index, dword_t ui_type,
                                            qword_t offer_id,
                                            dword_t offer_type,
                                            dword_t content_category,
                                            unknown_t unk6, unknown_t unk7,
                                            dword_t title_id) {
  // ui_type:
  // 0 - view all content for the current title
  // 1 - view content specified by offer id
  // offer_types:
  // filter for content list, usually just -1
  // content_category:
  // filter on item types for games (e.g. cars, maps, weapons, etc)
  if (user_index >= XUserMaxUserCount || ui_type >= MaxEntrypoint) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }

  bool is_xbla_unlock_offer =
      (offer_id == ((uint64_t(kernel_state()->title_id()) << 32) | 1ull));

  auto on_answer = [ui_type, is_xbla_unlock_offer](uint32_t button) -> void {
    if (ui_type == 1 && is_xbla_unlock_offer) {
      if (button == 0) {
        cvars::license_mask = 1;

        kernel_state()->BroadcastNotification(
            kXNotificationLiveContentInstalled, 0);
      }
    }
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  std::vector<std::string> buttons;
  // Not unlocked unattended.
  uint32_t unattended_button = 0;

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
      desc =
          "Game requested to open marketplace page with all content for the "
          "current title ID.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      desc = fmt::format(
          "Game requested to open marketplace page for offer ID 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipList:
      desc =
          "Game requested to open marketplace page with all Xbox Live "
          "memberships.";
      break;
    case X_MARKETPLACE_ENTRYPOINT::MembershipItem:
      desc = fmt::format(
          "Game requested to open marketplace page for an Xbox Live "
          "membership offer 0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentList_Background:
      // Used when accessing microsoft points
      desc = fmt::format(
          "Xbox Marketplace requested access to Microsoft Points offer page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem_Background:
      // Used when accessing credit card information and calls
      // XamShowCreditCardUI
      desc = fmt::format(
          "Xbox Marketplace requested access to credit card information page "
          "0x{:016X}.",
          static_cast<uint64_t>(offer_id));
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV1:
      // Used by XamShowForcedNameChangeUI v1888
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2:
      // Used by XamShowForcedNameChangeUI NXE and up
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::GamerTagPurchase:
      // Used by dashboard when selecting change gamertag in profile menu
      desc = fmt::format("Changing gamertag currently not implemented.");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ActiveDownloads:
      // Used in profile tabs when clicking active downloads
      desc = fmt::format(
          "There are no current plans to download files from Xbox "
          "Marketplace.");
      break;
    default:
      desc = fmt::format("Unknown marketplace op {:d}",
                         static_cast<uint32_t>(ui_type));
      break;
  }

  desc +=
      "\nNote that since Xenia cannot access Xbox Marketplace, any DLC must be "
      "installed manually using File -> Install Content.";

  switch (ui_type) {
    case X_MARKETPLACE_ENTRYPOINT::ContentList:
    default:
      buttons.push_back("OK");
      break;
    case X_MARKETPLACE_ENTRYPOINT::ContentItem:
      if (is_xbla_unlock_offer) {
        desc +=
            "\n\nTo start trial games in full mode, set license_mask to 1 in "
            "Xenia config file.\n\nDo you wish to change license_mask to 1 for "
            "*this session*?";
        buttons.push_back("Yes");
        buttons.push_back("No");
        unattended_button = 1;
      } else {
        buttons.push_back("OK");
      }
      break;
  }

  return xeXamShowChoiceAsync(
      {title, desc, std::move(buttons), 0, unattended_button}, on_answer);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUIEx, kUI, kSketchy);

dword_result_t XamShowMarketplaceUI_entry(dword_t user_index, dword_t ui_type,
                                          qword_t offer_id, dword_t offer_type,
                                          dword_t content_category,
                                          dword_t title_id) {
  return XamShowMarketplaceUIEx_entry(user_index, ui_type, offer_id, offer_type,
                                      content_category, 0, 0, title_id);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowMarketplaceDownloadItemsUI_entry(
    dword_t user_index, dword_t ui_type, lpqword_t offers, dword_t num_offers,
    lpdword_t hresult_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount || !offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                  X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto on_answer = [hresult_ptr](uint32_t) -> X_RESULT {
    if (hresult_ptr) {
      // TODO
      *hresult_ptr = X_E_SUCCESS;
    }
    return X_ERROR_SUCCESS;
  };

  std::string title = "Xbox Marketplace";
  std::string desc = "";
  std::vector<std::string> buttons = {"OK"};

  switch (ui_type) {
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::FREEITEMS:
      desc =
          "Game requested to open download page for the following free offer "
          "IDs:";
      break;
    case X_MARKETPLACE_DOWNLOAD_ITEMS_ENTRYPOINTS::PAIDITEMS:
      desc =
          "Game requested to open download page for the following offer IDs:";
      break;
    default:
      return X_ERROR_INVALID_PARAMETER;
  }

  for (uint32_t i = 0; i < num_offers; i++) {
    desc += fmt::format("\n0x{:16X}", offers[i].get());
  }

  desc +=
      "\n\nNote that since Xenia cannot access Xbox Marketplace, any DLC "
      "must "
      "be installed manually using File -> Install Content.";

  return xeXamShowChoice({title, desc, std::move(buttons)}, on_answer,
                         overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceDownloadItemsUI, kUI, kSketchy);

dword_result_t XamShowGoldUpgradeUI_entry(dword_t user_index, dword_t unk,
                                          dword_t title_id) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowGoldUpgradeUI, kUI, kStub);

dword_result_t XamShowMultiplayerUpgradeUI_entry(dword_t user_index) {
  return XamShowGoldUpgradeUI_entry(user_index, 6,
                                    kernel_state()->emulator()->title_id());
}
DECLARE_XAM_EXPORT1(XamShowMultiplayerUpgradeUI, kUI, kImplemented);

dword_result_t XamShowForcedNameChangeUI_entry(dword_t user_index) {
  return XamShowMarketplaceUIEx_entry(
      user_index, X_MARKETPLACE_ENTRYPOINT::ForcedNameChangeV2, 0, 0xffffffff,
      0, 0, 0, 0);
}
DECLARE_XAM_EXPORT1(XamShowForcedNameChangeUI, kUI, kImplemented);

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid) {
  const ImVec2 start_position = ImGui::GetCursorPos();

  ImGui::BeginGroup();
  {
    if (profile_icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(profile_icon),
                   xe::ui::default_image_icon_size);
    } else {
      if (user_index < XUserMaxUserCount) {
        const auto icon = imgui_drawer->GetNotificationIcon(user_index);
        ImGui::Image(reinterpret_cast<ImTextureID>(icon),
                     xe::ui::default_image_icon_size);
      } else {
        ImGui::Dummy(xe::ui::default_image_icon_size);
      }
    }

    ImGui::SameLine();

    ImGui::BeginGroup();
    {
      ImGui::TextUnformatted(
          fmt::format("User: {}\n", account->GetGamertagString()).c_str());
      ImGui::TextUnformatted(fmt::format("XUID: {:016X}  \n", xuid).c_str());
      if (user_index != XUserIndexAny) {
        ImGui::TextUnformatted(
            fmt::format("Assigned to slot: {}\n", user_index + 1).c_str());
      } else {
        ImGui::TextUnformatted(fmt::format("Profile is not signed in").c_str());
      }
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();

  if (xuid && selected_xuid) {
    const ImVec2 end_draw_position =
        ImVec2(ImGui::GetCursorPos().x - start_position.x,
               ImGui::GetCursorPos().y - start_position.y);

    ImGui::SetCursorPos(start_position);
    if (ImGui::Selectable("##Selectable", *selected_xuid == xuid,
                          ImGuiSelectableFlags_SpanAllColumns,
                          end_draw_position)) {
      *selected_xuid = xuid;
      ImGui::OpenPopup("Profile Menu");
    }

    if (context_menu) {
      return context_menu();
    }
  }

  return true;
}

X_RESULT xeXamShowSigninUI(uint32_t user_index, uint32_t users_needed,
                           uint32_t flags) {
  // Mask values vary. Probably matching user types? Local/remote?
  // Games seem to sit and loop until we trigger sign in notification.
  if (users_needed != 1 && users_needed != 2 && users_needed != 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  SignInRequest request;
  request.users_needed = users_needed;
  request.last_used_slot = static_cast<uint8_t>(
      kernel_state()->emulator()->input_system()->GetLastUsedSlot());
  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    UserProfile* profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile) {
      request.signed_in[static_cast<uint8_t>(i)] = profile->xuid();
    }
  }
  return xeXamShowDialogAsync<SignInAnswer>(
      [request = std::move(request)](std::function<void(SignInAnswer)> done) {
        kernel_state()->emulator()->host().RequestSignIn(request,
                                                         std::move(done));
      },
      [](SignInAnswer xuids) {
        if (xuids) {
          kernel_state()->xam_state()->profile_manager()->LoginMultiple(*xuids);
        }
      });
}

X_RESULT xeXamShowCreateProfileUIEx(uint32_t user_index, dword_t flag,
                                    char* unkn2_ptr) {
  // The answer carries nothing: the profile, if made, is the host's.
  return xeXamShowDialogAsync<bool>(
      [](std::function<void(bool)> done) {
        kernel_state()->emulator()->host().RequestProfileCreation(
            [done]() { done(true); });
      },
      [](bool) {});
}

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t flags) {
  return xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kImplemented);

dword_result_t XamShowSigninUIEx_entry(
    dword_t users_needed, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = xeXamShowSigninUI(XUserIndexAny, users_needed, flags);
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamShowSigninUIEx, kUserProfiles, kSketchy);

dword_result_t XamShowNuiSigninUI_entry(dword_t tracking_id, dword_t user_index,
                                        dword_t flags) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  uint32_t users_needed = 1;
  uint32_t sent_flags = flags | static_cast<uint32_t>(SigninUiFlags::NUI);
  // xeXamNuiHudCheck(tracking_id) = success then continue else return
  return xeXamShowSigninUI(user_index, users_needed, sent_flags);
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kUserProfiles, kSketchy);

dword_result_t XamShowSigninUIp_entry(dword_t user_index, dword_t users_needed,
                                      dword_t flags) {
  return xeXamShowSigninUI(user_index, users_needed, flags);
}
DECLARE_XAM_EXPORT1(XamShowSigninUIp, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUIEx_entry(dword_t user_index, dword_t flag,
                                              lpstring_t unkn2_ptr) {
  return xeXamShowCreateProfileUIEx(user_index, flag, unkn2_ptr);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUIEx, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUI_entry(dword_t user_index, dword_t flag) {
  return xeXamShowCreateProfileUIEx(user_index, flag, 0);
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowAchievementsUI_entry(dword_t user_index,
                                           dword_t title_id) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }

  uint32_t proper_title_id =
      title_id ? title_id.value()
               : kernel_state()->xam_state()->spa_info()->title_id();

  const auto info =
      kernel_state()->xam_state()->user_tracker()->GetUserTitleInfo(
          user->xuid(), proper_title_id);

  if (!info) {
    return X_ERROR_NO_SUCH_USER;
  }

  return xeXamShowDialogAsync<bool>(
      [xuid = user->xuid(), proper_title_id](std::function<void(bool)> done) {
        kernel_state()->emulator()->host().ShowAchievements(
            xuid, proper_title_id, [done]() { done(true); });
      },
      [](bool) {});
}
DECLARE_XAM_EXPORT1(XamShowAchievementsUI, kUserProfiles, kStub);

// Both the gamercard and its editor show the editable gamercard.
X_RESULT xeXamShowGamercard(uint64_t xuid) {
  return xeXamShowDialogAsync<bool>(
      [xuid](std::function<void(bool)> done) {
        kernel_state()->emulator()->host().ShowGamercard(
            xuid, [done]() { done(true); });
      },
      [](bool) {});
}

dword_result_t XamShowGamerCardUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  return xeXamShowGamercard(user->xuid());
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUI, kUserProfiles, kImplemented);

dword_result_t XamShowEditProfileUI_entry(dword_t user_index) {
  auto user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_ACCESS_DENIED;
  }

  return xeXamShowGamercard(user->xuid());
}
DECLARE_XAM_EXPORT1(XamShowEditProfileUI, kUserProfiles, kImplemented);

// UI
dword_result_t XamShowNuiTroubleshooterUI_entry(dword_t user_index,
                                                dword_t tracking_id,
                                                dword_t flags) {
  /* Notes:
     - calls XamPackageManagerGetExperienceMode(&var) with var = 1
     - If returns less than zero or (var & 1) == 0 then get error message:
       - if XamPackageManagerGetExperienceMode = 0 then call XamShowMessageBoxUI
         - if XamShowMessageBoxUI returns 0x3e5 then XamShowNuiTroubleshooterUI
     returns 0
       - else XamShowNuiTroubleshooterUI returns 0x65b and call another func
     - else:
       - call XamNuiHudSetEngagedTrackingID(tracking_id) and doesn't care aboot
     return and set var2 = 2
       - checks if (flag & 0x800000) == 0
         - if true call XamNuiGetDeviceStatus.
           - if XamNuiGetDeviceStatus != 0 set var2 = 3
       - else var2 = 4
       - XamAppRequestLoadEx(var2);
       - if return = 0 then XamShowNuiTroubleshooterUI returns 5
       - else set buffer[8] and call
     XMsgSystemProcessCall(0xfe,0x21028,buffer,0xc);
     - XamNuiNatalCameraUpdateComplete calls
     XamShowNuiTroubleshooterUI(0xff,0,0) if param = -0x7ff8fffe
  */

  xeXamShowChoice(
      {"NUI Troubleshooter",
       "The game has indicated there is a problem with NUI (Kinect).",
       {"OK"}},
      [](uint32_t) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

dword_result_t XamShowNuiHardwareRequiredUI_entry(unknown_t unk1) {
  if (unk1 != 0) {
    return X_ERROR_INVALID_PARAMETER;
  }

  return XamShowNuiTroubleshooterUI_entry(0xff, 0, 0x400000);
}
DECLARE_XAM_EXPORT1(XamShowNuiHardwareRequiredUI, kNone, kImplemented);

dword_result_t XamShowNuiGuideUI_entry(unknown_t tracking_id, unknown_t unk2) {
  auto result = kernel_state()->nui()->NuiHudCheck(tracking_id);
  if (result) {
    return result;
  }
  /* Notes:
   - calls xeXamNuiHudCheck, if success call XamAppRequestLoadEx(4) and if
   succeed call XMsgSystemProcessCall and some sub function that ensures correct
   h_result is returned
   - if XamAppRequestLoadEx(4) fails return X_ERROR_ACCESS_DENIED
  */

  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamShowNuiGuideUI, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(UI);
