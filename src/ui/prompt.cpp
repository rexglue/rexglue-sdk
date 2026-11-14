#include "ui/prompt.h"

#include <format>

#include <imgui.h>

#include <rex/string/text.h>

namespace rex::ui {

namespace {

constexpr u32 kDefaultTextCapacity = 1024;

}  // namespace

PromptDialog::PromptDialog(PromptConfig config, PromptDone done)
    : config_(std::move(config)), done_(std::move(done)) {
  if (config_.title.empty()) {
    if (config_.text_input && !config_.body.empty()) {
      config_.title = std::move(config_.body);
      config_.body.clear();
    } else {
      config_.title = config_.text_input ? "Keyboard Input" : "Message Box";
    }
  }
  if (config_.text_input) {
    buttons_ = {"OK", "Cancel"};
    buffer_.assign((config_.max_length != 0 ? config_.max_length : kDefaultTextCapacity) + 1, '\0');
    string::CopyTruncating(buffer_.data(), config_.default_text, buffer_.size());
  } else {
    buttons_ = config_.buttons.empty() ? std::vector<std::string>{"OK"} : config_.buttons;
  }
  label_ = std::format("{}###prompt{}", config_.title, reinterpret_cast<u64>(this));
}

PromptDialog::~PromptDialog() {
  Complete({});
}

void PromptDialog::Complete(PromptResult result) {
  done_(std::move(result));
}

void PromptDialog::Choose(u32 button, std::string text) {
  Complete(PromptResult{.canceled = false, .button = button, .text = std::move(text)});
  closing_ = true;
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
    closing_ = true;
    return;
  }
  if (accepted) {
    ImGui::CloseCurrentPopup();
    Choose(0, GetText());
  }
}

bool PromptDialog::Draw(ImGuiIO& io) {
  (void)io;
  bool first_draw = false;
  if (!opened_) {
    ImGui::OpenPopup(label_.c_str());
    opened_ = true;
    first_draw = true;
  }
  if (!ImGui::BeginPopupModal(label_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    Complete({});
    closing_ = true;
    return false;
  }
  if (!config_.body.empty()) {
    ImGui::TextWrapped("%s", config_.body.c_str());
  }
  if (first_draw) {
    ImGui::SetKeyboardFocusHere();
  }
  if (config_.text_input) {
    DrawInput();
  } else {
    DrawButtons();
  }
  ImGui::Spacing();
  ImGui::EndPopup();
  return !closing_;
}

}  // namespace rex::ui
