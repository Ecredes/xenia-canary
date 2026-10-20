/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/passcode_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

ProfilePasscodeUI::ProfilePasscodeUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                     const PasscodeRequest& request,
                                     std::function<void(PasscodeAnswer)> done)
    : XamDialog(imgui_drawer),
      title_(request.title),
      description_(request.description),
      done_(std::move(done)) {
  if (title_.empty()) {
    title_ = "Enter Pass Code";
  }

  if (description_.empty()) {
    description_ = "Enter your Xbox LIVE pass code.";
  }
}

ProfilePasscodeUI::~ProfilePasscodeUI() {
  if (done_) {
    done_(std::move(answer_));
  }
}

void ProfilePasscodeUI::DrawPasscodeField(uint8_t key_id) {
  const std::string label = fmt::format("##Key {}", key_id);

  if (ImGui::BeginCombo(label.c_str(), labelled_keys_[key_indexes_[key_id]])) {
    for (uint8_t key_index = 0; key_index < keys_map_.size(); key_index++) {
      bool is_selected = key_id == key_index;

      if (ImGui::Selectable(labelled_keys_[key_index], is_selected)) {
        key_indexes_[key_id] = key_index;
      }

      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }

    ImGui::EndCombo();
  }
}

void ProfilePasscodeUI::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (description_.size()) {
      ImGui::Text("%s", description_.c_str());
    }

    for (uint8_t i = 0; i < passcode_length; i++) {
      DrawPasscodeField(i);
    }

    ImGui::NewLine();

    if (ImGui::Button("Sign In")) {
      std::vector<uint16_t> keys;
      for (uint8_t i = 0; i < passcode_length; i++) {
        keys.push_back(keys_map_.at(labelled_keys_[key_indexes_[i]]));
      }
      answer_ = std::move(keys);

      Close();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel")) {
      Close();
    }

    ImGui::EndPopup();
  } else {
    Close();
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
