/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/choice_dialog.h"

#include <utility>

#include "third_party/imgui/imgui.h"

namespace xe {
namespace app {

ChoiceDialog::ChoiceDialog(ui::ImGuiDrawer* imgui_drawer, ChoiceRequest request,
                           std::function<void(uint32_t button)> done)
    : ui::ImGuiDialog(imgui_drawer),
      request_(std::move(request)),
      done_(std::move(done)),
      chosen_button_(request_.default_button) {
  if (request_.title.empty()) {
    request_.title = "Message Box";
  }
}

ChoiceDialog::~ChoiceDialog() {
  if (done_) {
    done_(chosen_button_);
  }
}

void ChoiceDialog::OnDraw(ImGuiIO& io) {
  bool first_draw = false;
  if (!has_opened_) {
    ImGui::OpenPopup(request_.title.c_str());
    has_opened_ = true;
    first_draw = true;
  }
  if (ImGui::BeginPopupModal(request_.title.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (request_.text.size()) {
      ImGui::Text("%s", request_.text.c_str());
    }
    if (first_draw) {
      ImGui::SetKeyboardFocusHere();
    }
    for (size_t i = 0; i < request_.buttons.size(); ++i) {
      if (ImGui::Button(request_.buttons[i].c_str())) {
        chosen_button_ = static_cast<uint32_t>(i);
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }
}

}  // namespace app
}  // namespace xe
