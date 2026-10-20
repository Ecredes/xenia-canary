/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TEXT_INPUT_DIALOG_H_
#define XENIA_APP_TEXT_INPUT_DIALOG_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xenia/emulator_host.h"
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

// A TextRequest as a modal ImGui popup with a text field.
class TextInputDialog final : public ui::ImGuiDialog {
 public:
  TextInputDialog(ui::ImGuiDrawer* imgui_drawer, TextRequest request,
                  std::function<void(std::optional<std::string> text)> done);
  // Answers here, so whoever waits is released even if the dialog is
  // destroyed without an answer (as cancelled).
  ~TextInputDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Submit();

  std::string title_;
  std::string description_;
  std::function<void(std::optional<std::string> text)> done_;
  bool has_opened_ = false;
  std::vector<char> text_buffer_;
  std::optional<std::string> text_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_TEXT_INPUT_DIALOG_H_
