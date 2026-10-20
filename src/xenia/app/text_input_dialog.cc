/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/text_input_dialog.h"

#include <utility>

#include "third_party/imgui/imgui.h"
#include "xenia/base/string_util.h"

namespace xe {
namespace app {

TextInputDialog::TextInputDialog(
    ui::ImGuiDrawer* imgui_drawer, TextRequest request,
    std::function<void(std::optional<std::string> text)> done)
    : ui::ImGuiDialog(imgui_drawer),
      title_(std::move(request.title)),
      description_(std::move(request.description)),
      done_(std::move(done)) {
  if (title_.empty()) {
    if (description_.empty()) {
      title_ = "Keyboard Input";
    } else {
      title_ = std::move(description_);
      description_.clear();
    }
  }
  text_buffer_.resize(request.max_length);
  xe::string_util::copy_truncating(text_buffer_.data(), request.default_text,
                                   text_buffer_.size());
}

TextInputDialog::~TextInputDialog() {
  if (done_) {
    done_(std::move(text_));
  }
}

void TextInputDialog::Submit() {
  // Up to the terminator, not the whole buffer.
  text_ = std::string(text_buffer_.data());
  ImGui::CloseCurrentPopup();
  Close();
}

void TextInputDialog::OnDraw(ImGuiIO& io) {
  bool first_draw = false;
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
    first_draw = true;
  }
  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (description_.size()) {
      ImGui::TextWrapped("%s", description_.c_str());
    }
    if (first_draw) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushID("input_text");
    bool input_submitted =
        ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    // Context menu for paste functionality
    if (ImGui::BeginPopupContextItem("input_context_menu")) {
      if (ImGui::MenuItem("Paste")) {
        if (ImGui::GetClipboardText() != nullptr) {
          std::string clipboard_text = ImGui::GetClipboardText();
          xe::string_util::copy_truncating(text_buffer_.data(), clipboard_text,
                                           text_buffer_.size());
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    if (input_submitted) {
      Submit();
    }
    if (ImGui::Button("OK")) {
      Submit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      text_.reset();
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::Spacing();
    ImGui::EndPopup();
  } else {
    Close();
  }
}

}  // namespace app
}  // namespace xe
