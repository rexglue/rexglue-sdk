#include "ui/sdl3/prompts.h"

#include <format>

#include <imgui.h>

#include <rex/string.h>

namespace rex::ui {

namespace {

constexpr u32 kDefaultTextCapacity = 1024;

}  // namespace

PromptDialog::PromptDialog(PromptDesc desc, PromptDone done)
    : desc_(std::move(desc)), done_(std::move(done)) {
  if (desc_.title.empty()) {
    if (desc_.text_input && !desc_.body.empty()) {
      desc_.title = std::move(desc_.body);
      desc_.body.clear();
    } else {
      desc_.title = desc_.text_input ? "Keyboard Input" : "Message Box";
    }
  }
  if (desc_.text_input) {
    buttons_ = {"OK", "Cancel"};
    buffer_.assign((desc_.max_length != 0 ? desc_.max_length : kDefaultTextCapacity) + 1, '\0');
    CopyTruncating(buffer_.data(), desc_.default_text, buffer_.size());
  } else {
    buttons_ = desc_.buttons.empty() ? std::vector<std::string>{"OK"} : desc_.buttons;
  }
  label_ = std::format("{}###prompt{}", desc_.title, reinterpret_cast<u64>(this));
}

PromptDialog::~PromptDialog() {
  Complete({});
}

void PromptDialog::Complete(PromptResult result) {
  done_(std::move(result));
}

void PromptDialog::Choose(u32 button, std::string text) {
  Complete(PromptResult{.canceled = false, .button = button, .text = std::move(text)});
  Close();
}

std::string PromptDialog::GetText() const {
  return std::string(buffer_.data());
}

void PromptDialog::DrawButtons() {
  for (size_t index = 0; index < buttons_.size(); ++index) {
    if (index != 0) {
      ImGui::SameLine();
    }
    if (ImGui::Button(std::format("{}##{}", buttons_[index], index).c_str())) {
      ImGui::CloseCurrentPopup();
      Choose(static_cast<u32>(index), {});
      return;
    }
  }
}

void PromptDialog::DrawInput() {
  const bool entered = ImGui::InputText("##body", buffer_.data(), buffer_.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
  bool accepted = entered;
  if (ImGui::Button("OK")) {
    accepted = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
    Complete({});
    Close();
    return;
  }
  if (accepted) {
    ImGui::CloseCurrentPopup();
    Choose(0, GetText());
  }
}

void PromptDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  bool first_draw = false;
  if (!opened_) {
    ImGui::OpenPopup(label_.c_str());
    opened_ = true;
    first_draw = true;
  }
  if (!ImGui::BeginPopupModal(label_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    Complete({});
    Close();
    return;
  }
  if (!desc_.body.empty()) {
    ImGui::TextWrapped("%s", desc_.body.c_str());
  }
  if (first_draw) {
    ImGui::SetKeyboardFocusHere();
  }
  if (desc_.text_input) {
    DrawInput();
  } else {
    DrawButtons();
  }
  ImGui::Spacing();
  ImGui::EndPopup();
}

}  // namespace rex::ui
