/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CHOICE_DIALOG_H_
#define XENIA_APP_CHOICE_DIALOG_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xenia/emulator_host.h"
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

// A ChoiceRequest as a modal ImGui popup of buttons.
class ChoiceDialog final : public ui::ImGuiDialog {
 public:
  ChoiceDialog(ui::ImGuiDrawer* imgui_drawer, ChoiceRequest request,
               std::function<void(uint32_t button)> done);
  // Answers here, so whoever waits is released even if the dialog is
  // destroyed without a choice.
  ~ChoiceDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ChoiceRequest request_;
  std::function<void(uint32_t button)> done_;
  bool has_opened_ = false;
  uint32_t chosen_button_ = 0;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_CHOICE_DIALOG_H_
