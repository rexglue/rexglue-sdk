#include "ui/overlay/console/view.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <imgui_internal.h>

#include <rex/ui/style.h>

namespace rex::ui {

namespace {

Color GetLevelStyleColor(const ConsoleStyle& style, log::Level level) {
  const size_t index = std::to_underlying(level);
  return index < style.level.size() ? style.level[index]
                                    : style.level[std::to_underlying(log::Level::kInfo)];
}

ImGuiID LineID(const ConsoleLine& line) {
  return static_cast<ImGuiID>(line.id);
}

void TextDisabled(std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  ImGui::PopStyleColor();
}

}  // namespace

ConsoleView::ConsoleView(ConsoleModel& model, IConsoleHost& host) : model_(model), host_(host) {
  selection_.UserData = &visible_;
  selection_.AdapterIndexToStorageId = [](ImGuiSelectionBasicStorage* self, int index) {
    const auto& lines = *static_cast<const std::vector<const ConsoleLine*>*>(self->UserData);
    return LineID(*lines[static_cast<size_t>(index)]);
  };
}

void ConsoleView::Draw(const ConsoleStyle& style) {
  model_.Poll();
  visible_ = model_.GetVisibleLines();
  DrawFilters();

  const f32 footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
  if (ImGui::BeginChild("##output", ImVec2(0.0f, -footer), ImGuiChildFlags_None,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    DrawOutput(style);
  }
  ImGui::EndChild();

  if (input_active_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##escape"), ImGuiInputFlags_LockThisFrame);
    if (!completions_.empty() && !dismissed_) {
      dismissed_ = true;
      highlight_ = -1;
    } else {
      host_.Hide();
    }
  }

  constexpr ImGuiInputTextFlags kInputFlags =
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
      ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit |
      ImGuiInputTextFlags_CallbackResize;
  std::string& input = model_.GetInput();
  ImGui::SetNextItemWidth(-1.0f);
  if (focus_input_) {
    ImGui::SetKeyboardFocusHere();
    focus_input_ = false;
  }
  const bool entered = ImGui::InputText("##input", input.data(), input.capacity() + 1, kInputFlags,
                                        InputCallback, this);
  input_active_ = ImGui::IsItemActive();
  const ImVec2 input_min = ImGui::GetItemRectMin();
  const f32 input_width = ImGui::GetItemRectSize().x;

  if (edited_) {
    edited_ = false;
    completions_ = model_.Complete(input);
    highlight_ = -1;
  }
  const bool open = !completions_.empty() && !dismissed_;
  if (entered) {
    if (open && highlight_ >= 0) {
      AcceptSelected(completions_[static_cast<size_t>(highlight_)].name);
    } else {
      model_.Execute(input);
      input.clear();
      CloseCompletions();
      scroll_to_bottom_ = true;
    }
    focus_input_ = true;
  }

  const bool still_open = !completions_.empty() && !dismissed_;
  const std::string help = model_.HelpLine(
      still_open && highlight_ >= 0 ? completions_[static_cast<size_t>(highlight_)].name : input);
  TextDisabled(help);
  if (still_open) {
    DrawCompletions(style, input_min, input_width);
  }
}

void ConsoleView::DrawFilters() {
  i32 level = std::to_underlying(model_.GetMinLevel());
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("critical").x + ImGui::GetFrameHeight() * 2.0f);
  if (ImGui::Combo(
          "##level", &level,
          [](void*, i32 index) { return log::kLevelNames[static_cast<size_t>(index)].data(); },
          nullptr, static_cast<i32>(log::kMessageLevelCount))) {
    model_.SetMinLevel(static_cast<log::Level>(level));
  }
  ImGui::SameLine();
  if (ImGui::Button("Categories")) {
    ImGui::OpenPopup("##categories");
  }
  if (ImGui::BeginPopup("##categories")) {
    for (const std::string& category : model_.GetCategories()) {
      bool shown = model_.IsShown(category);
      if (ImGui::Checkbox(category.c_str(), &shown)) {
        model_.SetShown(category, shown);
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    model_.Clear();
    visible_.clear();
    selection_.Clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy all")) {
    ImGui::SetClipboardText(SelectedText(true).c_str());
  }
  if (model_.GetDock() == ConsoleDock::kUndocked) {
    ImGui::SameLine();
    if (ImGui::Button("Dock")) {
      host_.Dock();
    }
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Minimize")) {
    model_.SetDock(ConsoleDock::kMinimized);
  }
  if (host_.CanUndock()) {
    ImGui::SameLine();
    if (ImGui::Button("Undock")) {
      host_.Undock();
    }
  }
}

void ConsoleView::DrawOutput(const ConsoleStyle& style) {
  const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
  const i32 count = static_cast<i32>(visible_.size());
  ImGuiMultiSelectIO* request = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_ClearOnEscape |
                                                            ImGuiMultiSelectFlags_ClearOnClickVoid |
                                                            ImGuiMultiSelectFlags_BoxSelect1d,
                                                        selection_.Size, count);
  selection_.ApplyRequests(request);

  ImGuiListClipper clipper;
  clipper.Begin(count);
  if (request->RangeSrcItem != -1) {
    clipper.IncludeItemByIndex(static_cast<i32>(request->RangeSrcItem));
  }
  const f32 width = ImGui::GetContentRegionAvail().x;
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  while (clipper.Step()) {
    for (i32 index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
      const ConsoleLine& line = *visible_[static_cast<size_t>(index)];
      const char* begin = line.text.data();
      const char* end = begin + line.text.size();
      const ImVec2 position = ImGui::GetCursorScreenPos();
      ImGui::PushID(index);
      ImGui::SetNextItemSelectionUserData(index);
      ImGui::Selectable("##line", selection_.Contains(LineID(line)), ImGuiSelectableFlags_None,
                        ImVec2(std::max(width, ImGui::CalcTextSize(begin, end).x), 0.0f));
      ImGui::PopID();
      draw_list->AddText(position,
                         ImGui::GetColorU32(ToImVec4(GetLevelStyleColor(style, line.level))), begin,
                         end);
    }
  }
  request = ImGui::EndMultiSelect();
  selection_.ApplyRequests(request);

  if (selection_.Size > 0 && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
    ImGui::SetClipboardText(SelectedText(false).c_str());
  }
  if (ImGui::BeginPopupContextWindow("##output_menu")) {
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, selection_.Size > 0)) {
      ImGui::SetClipboardText(SelectedText(false).c_str());
    }
    if (ImGui::MenuItem("Copy all")) {
      ImGui::SetClipboardText(SelectedText(true).c_str());
    }
    ImGui::EndPopup();
  }
  if (scroll_to_bottom_ || at_bottom) {
    ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
  }
}

void ConsoleView::DrawCompletions(const ConsoleStyle& style, ImVec2 input_min, f32 width) {
  size_t rows = completions_.size();
  for (size_t index = 0; index < completions_.size(); ++index) {
    if (index == 0 || completions_[index].scope != completions_[index - 1].scope) {
      ++rows;
    }
  }
  const f32 row = ImGui::GetTextLineHeightWithSpacing();
  const f32 height = std::min(
      static_cast<f32>(rows) * row + ImGui::GetStyle().WindowPadding.y * 2.0f, row * 12.0f);
  ImGui::SetCursorScreenPos(ImVec2(input_min.x, input_min.y - height));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ToImVec4(style.completion_bg));
  i32 clicked = -1;
  if (ImGui::BeginChild("##completions", ImVec2(width, height), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoFocusOnAppearing)) {
    if (ImGui::BeginTable("##candidates", 2, ImGuiTableFlags_None)) {
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch);
      for (size_t index = 0; index < completions_.size(); ++index) {
        const Completion& completion = completions_[index];
        if (index == 0 || completion.scope != completions_[index - 1].scope) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          TextDisabled(completion.scope.empty() ? std::string_view("console")
                                                : std::string_view(completion.scope));
        }
        ImGui::TableNextRow();
        const i32 position = static_cast<i32>(index);
        if (position == highlight_) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                 ImGui::GetColorU32(ToImVec4(style.completion_highlight)));
          ImGui::SetScrollHereY();
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(position);
        if (ImGui::Selectable(completion.name.c_str(), false,
                              ImGuiSelectableFlags_SpanAllColumns)) {
          clicked = position;
        }
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        TextDisabled(completion.description);
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (clicked >= 0) {
    AcceptSelected(completions_[static_cast<size_t>(clicked)].name);
  }
}

int ConsoleView::InputCallback(ImGuiInputTextCallbackData* data) {
  auto& view = *static_cast<ConsoleView*>(data->UserData);
  switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackResize: {
      std::string& input = view.model_.GetInput();
      input.resize(static_cast<size_t>(data->BufTextLen));
      data->Buf = input.data();
      break;
    }
    case ImGuiInputTextFlags_CallbackEdit:
      view.edited_ = true;
      view.dismissed_ = false;
      break;
    case ImGuiInputTextFlags_CallbackCompletion:
      view.CompleteInput(data);
      break;
    case ImGuiInputTextFlags_CallbackHistory:
      view.Navigate(data);
      break;
    default:
      break;
  }
  return 0;
}

void ConsoleView::CompleteInput(ImGuiInputTextCallbackData* data) {
  if (completions_.empty()) {
    return;
  }
  const bool picked = highlight_ >= 0 && !dismissed_;
  const bool full = picked || completions_.size() == 1;
  std::string text = completions_[picked ? static_cast<size_t>(highlight_) : 0].name;
  if (!full) {
    for (const Completion& completion : completions_) {
      const auto [mismatch, unused] = std::ranges::mismatch(text, completion.name);
      text.erase(mismatch, text.end());
    }
  }
  if (full) {
    text += ' ';
  }
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, text.data(), text.data() + text.size());
  if (full) {
    CloseCompletions();
  }
}

void ConsoleView::Navigate(ImGuiInputTextCallbackData* data) {
  const bool up = data->EventKey == ImGuiKey_UpArrow;
  if (!completions_.empty() && !dismissed_) {
    const i32 count = static_cast<i32>(completions_.size());
    highlight_ = up ? (highlight_ <= 0 ? count - 1 : highlight_ - 1) : (highlight_ + 1) % count;
    return;
  }
  const std::string_view entry = up ? model_.StepHistoryBack() : model_.StepHistoryForward();
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, entry.data(), entry.data() + entry.size());
  CloseCompletions();
}

void ConsoleView::AcceptSelected(const std::string& name) {
  model_.GetInput() = name + " ";
  CloseCompletions();
  focus_input_ = true;
}

void ConsoleView::CloseCompletions() {
  completions_.clear();
  highlight_ = -1;
  dismissed_ = false;
}

std::string ConsoleView::SelectedText(bool all) const {
  std::string text;
  for (const ConsoleLine* line : visible_) {
    if (all || selection_.Contains(LineID(*line))) {
      text += line->text;
      text += '\n';
    }
  }
  return text;
}

}  // namespace rex::ui
