#include <rex/ui/console/model.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <utility>

#include <rex/cvar.h>
#include <rex/cvar/registry.h>
#include <rex/platform/file.h>

namespace rex::ui {

namespace {

struct BuiltIn {
  std::string_view name;
  std::string_view description;
};

constexpr std::array<BuiltIn, 2> kBuiltIns = {{
    {"clear", "Empties the console output"},
    {"help", "Lists every cvar and command with its value"},
}};

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

std::string_view TypeName(cvar::Type type) {
  switch (type) {
    case cvar::Type::Bool:
      return "bool";
    case cvar::Type::Int32:
      return "i32";
    case cvar::Type::Int64:
      return "i64";
    case cvar::Type::UInt32:
      return "u32";
    case cvar::Type::UInt64:
      return "u64";
    case cvar::Type::Double:
      return "f64";
    case cvar::Type::String:
      return "string";
    case cvar::Type::Command:
      return "command";
  }
  return "value";
}

std::string JoinAllowed(std::span<const std::string_view> allowed) {
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

ConsoleModel::ConsoleModel(cvar::ICvarRegistry& cvars, log::CaptureSink& capture,
                           std::filesystem::path history_path)
    : cvars_(cvars), capture_(capture), history_path_(std::move(history_path)) {
  LoadHistory();
  history_pos_ = history_.size();
}

void ConsoleModel::Poll() {
  last_sequence_ = capture_.CopySince(last_sequence_, poll_buffer_);
  for (log::CapturedRecord& record : poll_buffer_) {
    categories_.insert(record.category);
    Append(ConsoleLine{record.level, std::move(record.category), std::move(record.text),
                       record.sequence});
  }
  poll_buffer_.clear();
}

void ConsoleModel::Append(ConsoleLine line) {
  std::string_view text = line.text;
  for (const auto part : std::views::split(text, '\n')) {
    lines_.push_back(ConsoleLine{line.level, line.category, std::string(std::string_view(part)),
                                 line.sequence, ++next_id_});
  }
  while (lines_.size() > kMaxLines) {
    lines_.pop_front();
  }
}

void ConsoleModel::Add(log::Level level, std::string text) {
  Append(ConsoleLine{level, "console", std::move(text), capture_.latest()});
}

void ConsoleModel::Clear() {
  lines_.clear();
}

void ConsoleModel::Execute(std::string_view line) {
  const std::string_view command = Trim(line);
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
  const std::string_view args =
      space == std::string_view::npos ? std::string_view{} : Trim(command.substr(space + 1));
  const cvar::IVar* var = cvars_.Find(name);
  if (var && var->desc().type == cvar::Type::Command) {
    if (auto invoked = cvars_.Invoke(name, args); !invoked) {
      Add(log::Level::kWarn, invoked.error().message);
    }
    return;
  }
  if (!var) {
    Add(log::Level::kWarn, std::format("unknown cvar {}", name));
    return;
  }
  if (args.empty()) {
    if (auto value = cvars_.Get(name)) {
      Add(log::Level::kInfo, std::format("{} = {}", name, *value));
    }
    return;
  }
  if (auto set = cvars_.Set(name, args); set) {
    Add(log::Level::kInfo,
        std::format("{} = {}", name, cvars_.Get(name).value_or(std::string(args))));
  } else {
    Add(log::Level::kWarn, set.error().message);
  }
}

void ConsoleModel::ListCvars() {
  std::vector<const cvar::IVar*> vars = cvars_.Enumerate();
  std::ranges::sort(vars, {}, [](const cvar::IVar* var) { return var->desc().name; });
  for (const cvar::IVar* var : vars) {
    const cvar::Desc& desc = var->desc();
    if (desc.type == cvar::Type::Command) {
      Add(log::Level::kInfo, std::format("  {}  {}", desc.name, desc.description));
    } else {
      Add(log::Level::kInfo,
          std::format("  {} = {}  {}", desc.name, var->Render(), desc.description));
    }
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
  for (const cvar::IVar* var : cvars_.Enumerate()) {
    const cvar::Desc& desc = var->desc();
    if (desc.name.starts_with(input)) {
      vars.push_back({std::string(desc.name), std::string(desc.scope),
                      std::string(desc.description), desc.type == cvar::Type::Command});
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
  const std::string_view trimmed = Trim(input);
  const std::string name(trimmed.substr(0, trimmed.find(' ')));
  if (name.empty()) {
    return {};
  }
  for (const BuiltIn& built_in : kBuiltIns) {
    if (built_in.name == name) {
      return std::format("{}. {}", name, built_in.description);
    }
  }
  const cvar::IVar* var = cvars_.Find(name);
  if (!var) {
    return {};
  }
  const cvar::Desc& desc = var->desc();
  if (desc.type == cvar::Type::Command) {
    return std::format("{} command. {}", name, desc.description);
  }
  std::string line = std::format("{} {} = {}, default {}", name, TypeName(desc.type), var->Render(),
                                 var->DefaultText());
  if (!desc.constraints.allowed.empty()) {
    line += std::format(", allowed {}", JoinAllowed(desc.constraints.allowed));
  }
  if (desc.constraints.min && desc.constraints.max) {
    line += std::format(", range {} to {}", *desc.constraints.min, *desc.constraints.max);
  }
  line += std::format(". {}", desc.description);
  return line;
}

std::string_view ConsoleModel::HistoryPrevious() {
  if (history_.empty()) {
    return {};
  }
  if (history_pos_ > 0) {
    --history_pos_;
  }
  return history_[history_pos_];
}

std::string_view ConsoleModel::HistoryNext() {
  if (history_pos_ < history_.size()) {
    ++history_pos_;
  }
  return history_pos_ < history_.size() ? std::string_view(history_[history_pos_])
                                        : std::string_view{};
}

std::vector<const ConsoleLine*> ConsoleModel::Visible() const {
  std::vector<const ConsoleLine*> out;
  out.reserve(lines_.size());
  for (const ConsoleLine& line : lines_) {
    if (line.level < min_level_ && line.category != "console") {
      continue;
    }
    if (line.category != "console" && !shown(line.category)) {
      continue;
    }
    out.push_back(&line);
  }
  return out;
}

bool ConsoleModel::shown(std::string_view category) const {
  return !hidden_.contains(category);
}

void ConsoleModel::set_shown(std::string_view category, bool shown) {
  if (shown) {
    if (auto it = hidden_.find(category); it != hidden_.end()) {
      hidden_.erase(it);
    }
  } else {
    hidden_.emplace(category);
  }
}

void ConsoleModel::set_size(f32 width, f32 height) {
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
  (void)platform::WriteWholeFile(history_path_,
                                 std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

}  // namespace rex::ui
