#include "ui/overlay/console/model.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <utility>

#include <rex/string/text.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/command.h>
#include <rex/logging/log.h>
#include <rex/platform/file.h>

namespace rex::ui {

namespace {

struct BuiltIn {
  std::string_view name;
  std::string_view description;
};

constexpr std::array<BuiltIn, 2> kBuiltIns = {{
    {"clear", "Empties the console output"},
    {"help", "Lists every setting and command with its value"},
}};

std::string_view TypeName(SettingType type) {
  switch (type) {
    case SettingType::kBool:
      return "bool";
    case SettingType::kI32:
      return "i32";
    case SettingType::kI64:
      return "i64";
    case SettingType::kU32:
      return "u32";
    case SettingType::kU64:
      return "u64";
    case SettingType::kF64:
      return "f64";
    case SettingType::kString:
      return "string";
    case SettingType::kPath:
      return "path";
  }
  return "value";
}

Command* FindCommand(ISettings& settings, std::string_view name) {
  for (Command* command : settings.EnumerateCommands()) {
    if (command->GetName() == name) {
      return command;
    }
  }
  return nullptr;
}

std::string JoinAllowed(std::span<const std::string> allowed) {
  std::string out;
  for (std::string_view value : allowed) {
    if (!out.empty()) {
      out += ' ';
    }
    out += value.empty() ? std::string_view("\"\"") : value;
  }
  return out;
}

}  // namespace

ConsoleModel::ConsoleModel(ISettings& settings, log::CaptureSink& capture,
                           std::filesystem::path history_path)
    : settings_(settings), capture_(capture), history_path_(std::move(history_path)) {
  LoadHistory();
  history_pos_ = history_.size();
}

void ConsoleModel::Poll() {
  last_sequence_ = capture_.CopySince(last_sequence_, poll_buffer_);
  for (log::CapturedMessage& message : poll_buffer_) {
    categories_.insert(message.category);
    Append(message.level, message.category, message.text, message.sequence);
  }
  poll_buffer_.clear();
}

void ConsoleModel::Append(log::Level level, std::string_view category, std::string_view text,
                          u64 sequence) {
  for (const auto part : std::views::split(text, '\n')) {
    lines_.push_back(ConsoleLine{level, std::string(category), std::string(std::string_view(part)),
                                 sequence, ++next_id_});
  }
  while (lines_.size() > kMaxLines) {
    lines_.pop_front();
  }
}

void ConsoleModel::Add(log::Level level, std::string_view text) {
  Append(level, "console", text, capture_.GetLatestSequence());
}

void ConsoleModel::Clear() {
  lines_.clear();
}

void ConsoleModel::Execute(std::string_view line) {
  const std::string_view command = string::Trim(line);
  if (command.empty()) {
    return;
  }
  AddHistory(command);
  Poll();
  if (command == "clear") {
    Clear();
    return;
  }
  Add(log::Level::kInfo, std::format("> {}", command));
  if (command == "help") {
    ListCvars();
    return;
  }
  const auto space = command.find(' ');
  const std::string name(command.substr(0, space));
  const std::string_view args = space == std::string_view::npos
                                    ? std::string_view{}
                                    : string::Trim(command.substr(space + 1));
  if (Command* found = FindCommand(settings_, name)) {
    if (auto invoked = found->Run(args); !invoked) {
      Add(log::Level::kWarn, invoked.error().message);
    }
    return;
  }
  const auto var = settings_.Find(name);
  if (!var) {
    Add(log::Level::kWarn, std::format("unknown setting {}", name));
    return;
  }
  if (args.empty()) {
    Add(log::Level::kInfo, std::format("{} = {}", name, (*var)->GetText()));
    return;
  }
  if (auto set = settings_.SetText(name, args); set) {
    Add(log::Level::kInfo, std::format("{} = {}", name, (*var)->GetText()));
  } else {
    Add(log::Level::kWarn, set.error().message);
  }
}

void ConsoleModel::ListCvars() {
  struct Row {
    std::string name;
    std::string text;
  };
  std::vector<Row> rows;
  for (const SettingBase* var : settings_.Enumerate()) {
    rows.push_back({var->GetName(),
                    std::format("  {} = {}  {}", var->GetName(), var->GetText(), var->GetHelp())});
  }
  for (const Command* command : settings_.EnumerateCommands()) {
    rows.push_back(
        {command->GetName(), std::format("  {}  {}", command->GetName(), command->GetHelp())});
  }
  std::ranges::sort(rows, {}, &Row::name);
  for (const Row& row : rows) {
    Add(log::Level::kInfo, row.text);
  }
}

std::vector<Completion> ConsoleModel::Complete(std::string_view input) const {
  std::vector<Completion> out;
  if (input.empty() || input.find(' ') != std::string_view::npos) {
    return out;
  }
  for (const BuiltIn& built_in : kBuiltIns) {
    if (built_in.name.starts_with(input)) {
      out.push_back({std::string(built_in.name), {}, std::string(built_in.description), true});
    }
  }
  std::vector<Completion> vars;
  for (const SettingBase* var : settings_.Enumerate()) {
    if (var->GetName().starts_with(input)) {
      vars.push_back({var->GetName(), var->GetOwner().GetScope(), var->GetHelp(), false});
    }
  }
  for (const Command* command : settings_.EnumerateCommands()) {
    if (command->GetName().starts_with(input)) {
      vars.push_back({command->GetName(),
                      command->GetName().substr(0, command->GetName().rfind('.')),
                      command->GetHelp(), true});
    }
  }
  std::ranges::sort(vars, [](const Completion& a, const Completion& b) {
    return std::tie(a.scope, a.name) < std::tie(b.scope, b.name);
  });
  out.insert(out.end(), std::make_move_iterator(vars.begin()), std::make_move_iterator(vars.end()));
  if (out.size() > kMaxCompletions) {
    out.resize(kMaxCompletions);
  }
  return out;
}

std::string ConsoleModel::HelpLine(std::string_view input) const {
  const std::string_view trimmed = string::Trim(input);
  const std::string name(trimmed.substr(0, trimmed.find(' ')));
  if (name.empty()) {
    return {};
  }
  for (const BuiltIn& built_in : kBuiltIns) {
    if (built_in.name == name) {
      return std::format("{}. {}", name, built_in.description);
    }
  }
  if (const Command* command = FindCommand(settings_, name)) {
    return std::format("{} command. {}", name, command->GetHelp());
  }
  const auto found = settings_.Find(name);
  if (!found) {
    return {};
  }
  const SettingBase* var = *found;
  const SettingConstraints& constraints = var->GetConstraints();
  std::string line = std::format("{} {} = {}, default {}", name, TypeName(var->GetType()),
                                 var->GetText(), var->GetDefaultText());
  if (!constraints.allowed.empty()) {
    line += std::format(", allowed {}", JoinAllowed(constraints.allowed));
  }
  if (constraints.min && constraints.max) {
    line += std::format(", range {} to {}", *constraints.min, *constraints.max);
  }
  line += std::format(". {}", var->GetHelp());
  return line;
}

std::string_view ConsoleModel::StepHistoryBack() {
  if (history_.empty()) {
    return {};
  }
  if (history_pos_ > 0) {
    --history_pos_;
  }
  return history_[history_pos_];
}

std::string_view ConsoleModel::StepHistoryForward() {
  if (history_pos_ < history_.size()) {
    ++history_pos_;
  }
  return history_pos_ < history_.size() ? std::string_view(history_[history_pos_])
                                        : std::string_view{};
}

std::vector<const ConsoleLine*> ConsoleModel::GetVisibleLines() const {
  std::vector<const ConsoleLine*> out;
  out.reserve(lines_.size());
  for (const ConsoleLine& line : lines_) {
    if (line.level < min_level_ && line.category != "console") {
      continue;
    }
    if (line.category != "console" && !IsShown(line.category)) {
      continue;
    }
    out.push_back(&line);
  }
  return out;
}

bool ConsoleModel::IsShown(std::string_view category) const {
  return !hidden_.contains(category);
}

void ConsoleModel::SetShown(std::string_view category, bool shown) {
  if (shown) {
    if (auto it = hidden_.find(category); it != hidden_.end()) {
      hidden_.erase(it);
    }
  } else {
    hidden_.emplace(category);
  }
}

void ConsoleModel::SetSize(f32 width, f32 height) {
  width_fraction_ = std::clamp(width, 0.2f, 1.0f);
  height_fraction_ = std::clamp(height, 0.1f, 1.0f);
}

void ConsoleModel::AddHistory(std::string_view line) {
  if (history_.empty() || history_.back() != line) {
    history_.emplace_back(line);
    while (history_.size() > kMaxHistory) {
      history_.pop_front();
    }
    SaveHistory();
  }
  history_pos_ = history_.size();
}

void ConsoleModel::LoadHistory() {
  if (history_path_.empty()) {
    return;
  }
  std::ifstream file(history_path_);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      history_.push_back(line);
    }
  }
  while (history_.size() > kMaxHistory) {
    history_.pop_front();
  }
}

void ConsoleModel::SaveHistory() const {
  if (history_path_.empty()) {
    return;
  }
  std::string text;
  for (const std::string& entry : history_) {
    text += entry;
    text += '\n';
  }
  if (auto written = platform::WriteHostFile(
          history_path_, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
      !written) {
    REXUI_WARN("console history not saved, {}", written.error().message);
  }
}

}  // namespace rex::ui
