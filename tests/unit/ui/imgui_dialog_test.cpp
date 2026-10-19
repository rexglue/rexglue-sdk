/**
 * @file        tests/unit/ui/imgui_dialog_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <imgui.h>

#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>

namespace {

class ImGuiFrame : public rex::NonCopyable {
 public:
  ImGuiFrame() {
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    rex::u8* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
  }

  ~ImGuiFrame() {
    ImGui::EndFrame();
    ImGui::DestroyContext(context_);
  }

 private:
  ImGuiContext* context_ = nullptr;
};

class RecordingHost final : public rex::ui::IDialogHost {
 public:
  void RemoveDialog(rex::ui::ImGuiDialog& dialog) override {
    ++removed;
    last = &dialog;
    dialog.Attach(nullptr);
  }

  rex::i32 removed = 0;
  rex::ui::ImGuiDialog* last = nullptr;
};

class CountingDialog final : public rex::ui::ImGuiDialog {
 public:
  using ImGuiDialog::Close;

  rex::i32 draws = 0;
  rex::i32 closes = 0;

 protected:
  void OnDraw(ImGuiIO&) override { ++draws; }
  void OnClose() override { ++closes; }
};

}  // namespace

REX_TEST_CASE("A closed dialog is removed from its host by the next Draw", "[ui][imgui]") {
  ImGuiFrame frame;
  RecordingHost host;
  CountingDialog dialog;
  dialog.Attach(&host);
  REQUIRE(dialog.IsAttached());
  REQUIRE_FALSE(dialog.IsClosed());
  dialog.Close();
  REQUIRE_FALSE(dialog.IsClosed());
  dialog.Draw();
  REQUIRE(dialog.draws == 1);
  REQUIRE(dialog.closes == 1);
  REQUIRE(host.removed == 1);
  REQUIRE(host.last == &dialog);
  REQUIRE(dialog.IsClosed());
  REQUIRE_FALSE(dialog.IsAttached());
  dialog.Draw();
  REQUIRE(dialog.closes == 1);
  REQUIRE(host.removed == 1);
}

REX_TEST_CASE("A dialog that never closes stays attached across Draw", "[ui][imgui]") {
  ImGuiFrame frame;
  RecordingHost host;
  CountingDialog dialog;
  dialog.Attach(&host);
  dialog.Draw();
  dialog.Draw();
  REQUIRE(dialog.draws == 2);
  REQUIRE(dialog.closes == 0);
  REQUIRE(host.removed == 0);
  REQUIRE(dialog.IsAttached());
  REQUIRE_FALSE(dialog.IsClosed());
}

REX_TEST_CASE("Destroying an attached dialog removes it from its host", "[ui][imgui]") {
  RecordingHost host;
  const rex::ui::ImGuiDialog* address = nullptr;
  {
    CountingDialog dialog;
    address = &dialog;
    dialog.Attach(&host);
  }
  REQUIRE(host.removed == 1);
  REQUIRE(host.last == address);
}

REX_TEST_CASE("Attach with null detaches without touching the host", "[ui][imgui]") {
  RecordingHost host;
  {
    CountingDialog dialog;
    dialog.Attach(&host);
    dialog.Attach(nullptr);
    REQUIRE_FALSE(dialog.IsAttached());
  }
  REQUIRE(host.removed == 0);
}
