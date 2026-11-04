#include "base/logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>

#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if REX_PLATFORM_WIN32
#include <spdlog/sinks/msvc_sink.h>
#endif

#include <rex/string/text.h>
#include <rex/logging/log.h>
#include <rex/platform/thread.h>
#include <rex/settings.h>

namespace rex::log {

class DetailAccess {
 public:
  static std::atomic<u16>& Slot(const Category& category) { return category.slot_; }
};

namespace {

thread_local bool t_in_sinks = false;
thread_local u64 t_sequence = 0;

struct InSinks {
  InSinks() { t_in_sinks = true; }
  ~InSinks() { t_in_sinks = false; }
};

CapturedMessage Capture(const Message& message) {
  return CapturedMessage{message.level,
                         std::string(message.category),
                         std::string(message.text),
                         message.time,
                         message.thread_id,
                         message.sequence};
}

Message View(const CapturedMessage& message) {
  return Message{message.level, message.category,  message.text,
                 message.time,  message.thread_id, message.sequence};
}

spdlog::level::level_enum ToSpdlog(Level level) {
  switch (level) {
    case Level::kTrace:
    case Level::kDev:
      return spdlog::level::trace;
    case Level::kDebug:
      return spdlog::level::debug;
    case Level::kInfo:
      return spdlog::level::info;
    case Level::kWarn:
      return spdlog::level::warn;
    case Level::kError:
      return spdlog::level::err;
    case Level::kCritical:
      return spdlog::level::critical;
    case Level::kOff:
      break;
  }
  return spdlog::level::off;
}

Level LevelOf(const spdlog::details::log_msg& message) {
  return static_cast<Level>(message.source.line);
}

spdlog::details::log_msg ToLogMessage(const Message& message) {
  spdlog::details::log_msg out(
      message.time,
      spdlog::source_loc(nullptr, static_cast<int>(std::to_underlying(message.level)), nullptr),
      message.category, ToSpdlog(message.level), message.text);
  out.thread_id = message.thread_id;
  return out;
}

Message ToMessage(const spdlog::details::log_msg& message, u64 sequence) {
  return Message{LevelOf(message),
                 message.logger_name,
                 message.payload,
                 message.time,
                 static_cast<u32>(message.thread_id),
                 sequence};
}

void SafeLog(spdlog::sinks::sink& sink, const spdlog::details::log_msg& message) noexcept {
  try {
    if (sink.should_log(message.level)) {
      sink.log(message);
    }
  } catch (...) {}
}

void SafeFlush(spdlog::sinks::sink& sink) noexcept {
  try {
    sink.flush();
  } catch (...) {}
}

class LetterFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& message, const std::tm&,
              spdlog::memory_buf_t& out) override {
    out.push_back(LevelLetter(LevelOf(message)));
  }

  std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<LetterFlag>();
  }
};

class ThreadFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& message, const std::tm&,
              spdlog::memory_buf_t& out) override {
    out.append(std::format("t{:05d}", message.thread_id));
  }

  std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<ThreadFlag>();
  }
};

constexpr std::string_view kBuiltinFlags = "vtPnlLaAbBcCYDxmdHIMSefFEprRTXz%+^$@sg#!oiuO";
constexpr std::string_view kRexFlags = "Qj";

std::unique_ptr<spdlog::formatter> MakeFormatter(const std::string& pattern) {
  auto formatter =
      std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::local, "\n");
  formatter->add_flag<LetterFlag>('Q').add_flag<ThreadFlag>('j');
  formatter->set_pattern(pattern);
  return formatter;
}

std::shared_ptr<spdlog::sinks::sink> MakeConsole(bool color, const std::string& pattern) {
  auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>(
      color ? spdlog::color_mode::always : spdlog::color_mode::never);
  sink->set_formatter(MakeFormatter(pattern));
  return sink;
}

std::shared_ptr<spdlog::sinks::sink> MakeDebugger([[maybe_unused]] const std::string& pattern) {
#if REX_PLATFORM_WIN32
  auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
  sink->set_formatter(MakeFormatter(pattern));
  return sink;
#else
  return nullptr;
#endif
}

/** Returns why a pattern cannot be used, empty when it can. */
std::string CheckPattern(std::string_view pattern) {
  for (size_t at = 0; at < pattern.size(); ++at) {
    if (pattern[at] != '%') {
      continue;
    }
    ++at;
    size_t probe = at;
    if (probe < pattern.size() && (pattern[probe] == '-' || pattern[probe] == '=')) {
      ++probe;
    }
    if (probe < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[probe]))) {
      while (probe < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[probe]))) {
        ++probe;
      }
      if (probe < pattern.size() && pattern[probe] == '!') {
        ++probe;
      }
      at = probe;
    }
    if (at >= pattern.size()) {
      return "the pattern ends inside a flag";
    }
    if (kBuiltinFlags.find(pattern[at]) == std::string_view::npos &&
        kRexFlags.find(pattern[at]) == std::string_view::npos) {
      return std::format("the pattern has the unknown flag %{}", pattern[at]);
    }
  }
  return {};
}

class ConsumerSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 public:
  ConsumerSink(ISink& target, const u64& sequence) : target_(target), sequence_(sequence) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    target_.Write(ToMessage(message, sequence_));
  }

  void flush_() override { target_.Flush(); }

 private:
  ISink& target_;
  const u64& sequence_;
};

}  // namespace

class SinkLogger final : public spdlog::logger {
 public:
  explicit SinkLogger(Logger::State& state) : spdlog::logger("rex"), state_(state) {}

  void Emit(const Message& message) {
    t_sequence = message.sequence;
    log_it_(ToLogMessage(message), true, false);
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    state_.Dispatch(message, t_sequence);
    if (should_flush_(message)) {
      flush_();
    }
  }

  void flush_() override { state_.FlushSinks(); }

 private:
  Logger::State& state_;
};

[[nodiscard]] Result<Level> ParseLevel(std::string_view text) {
  const std::string lower = string::utf8_lower_ascii(text);
  for (size_t index = 0; index < kLevelNames.size(); ++index) {
    if (kLevelNames[index] == lower) {
      return static_cast<Level>(index);
    }
  }
  return Err<Level>(ErrorCategory::kConfig, std::format("unknown log level '{}'", text));
}

std::string_view LevelName(Level level) {
  const auto index = static_cast<size_t>(std::to_underlying(level));
  return index < kLevelNames.size() ? kLevelNames[index] : kLevelNames.back();
}

bool Category::IsEnabled(Level level) const {
  Logger::State& logger = GetLoggerInstance();
  u16 slot = slot_.load(std::memory_order_acquire);
  if (slot == kUnresolved) {
    slot = logger.Resolve(*this);
  }
  return level != Level::kOff && level >= logger.GetSlotLevel(slot);
}

namespace detail {

void Write(const Category& category, Level level, std::string_view text) {
  Logger::State& logger = GetLoggerInstance();
  logger.Write(Message{level, category.GetName(), text, std::chrono::system_clock::now(),
                       platform::GetCallerSystemThreadID(),
                       logger.sequence.fetch_add(1, std::memory_order_relaxed) + 1});
}

void WriteArgs(const Category& category, Level level, std::string_view fmt,
               std::format_args args) noexcept {
  try {
    Write(category, level, std::vformat(fmt, args));
  } catch (const std::exception& error) {
    WriteFailure(category, error.what());
  } catch (...) {
    WriteFailure(category, "unknown exception");
  }
}

void WriteFailure(const Category& category, const char* what) noexcept {
  try {
    Write(category, Level::kError, std::format("log message dropped, {}", what));
  } catch (...) {
    std::fprintf(stderr, "log message dropped, %s\n", what);
  }
}

void WriteStub(const Category& category, std::atomic_flag& seen, std::string_view name,
               std::string_view detail) {
  const Level level = seen.test_and_set(std::memory_order_relaxed) ? Level::kDev : Level::kWarn;
  if (!category.IsEnabled(level)) {
    return;
  }
  Write(category, level,
        detail.empty() ? std::format("{} stub", name) : std::format("{} stub, {}", name, detail));
}

}  // namespace detail

void Flush() {
  REX_LOGGER().Flush();
}

}  // namespace rex::log

namespace rex {

namespace {

class CategoryEntry final : public SettingOwner {
 public:
  CategoryEntry(Logger::State& state, std::string_view name, std::string_view description)
      : SettingOwner("log.categories"),
        level(this, name, std::string(), description,
              Allowed("", "trace", "dev", "debug", "info", "warn", "error", "critical", "off")) {
    watch_ = level.OnChange([&state, name = std::string(name)](const std::string& text) {
      state.ApplyCategoryLevel(name, text);
    });
  }

  Setting<std::string> level;

 private:
  ScopeGuard watch_;
};

}  // namespace

Logger::State::State(Logger& logger)
    : owner(logger),
      engine(std::make_shared<log::SinkLogger>(*this)),
      pattern(logger.pattern.GetDefaultText()),
      start_time(std::chrono::system_clock::now()) {
  slots.reserve(log::kMaxCategories);
  for (auto& slot_level : levels) {
    slot_level.store(log::Level::kInfo, std::memory_order_relaxed);
  }
  engine->flush_on(spdlog::level::warn);
  CreateLocked(log::core.GetName(), log::core.GetDescription());
  console = log::MakeConsole(false, pattern);
  debugger = log::MakeDebugger(pattern);
  RebuildSinksLocked();
}

std::optional<u16> Logger::State::FindLocked(std::string_view name) const {
  for (size_t index = 0; index < slots.size(); ++index) {
    if (slots[index].name == name) {
      return static_cast<u16>(index);
    }
  }
  return std::nullopt;
}

u16 Logger::State::CreateLocked(std::string_view name, std::string_view description) {
  if (slots.size() >= log::kMaxCategories) {
    return 0;
  }
  Slot slot;
  slot.name = std::string(name);
  slot.description = std::string(description);
  slots.push_back(std::move(slot));
  const auto index = static_cast<u16>(slots.size() - 1);
  RecomputeLocked(index);
  return index;
}

void Logger::State::RecomputeLocked(u16 slot) {
  const log::Level resolved = slots[slot].explicit_level.value_or(default_level);
  levels[slot].store(resolved, std::memory_order_release);
}

void Logger::State::RecomputeAllLocked() {
  for (size_t index = 0; index < slots.size(); ++index) {
    RecomputeLocked(static_cast<u16>(index));
  }
}

u16 Logger::State::Resolve(const log::Category& category) {
  bool created = false;
  u16 slot = 0;
  {
    std::lock_guard lock(mutex);
    if (auto found = FindLocked(category.GetName())) {
      slot = *found;
    } else {
      slot = CreateLocked(category.GetName(), category.GetDescription());
      created = true;
    }
  }
  log::DetailAccess::Slot(category).store(slot, std::memory_order_release);
  if (created) {
    REXLOG_WARN("log category {} is not registered", category.GetName());
  }
  return slot;
}

Result<void> Logger::State::AddCategories(std::span<const log::Category* const> categories) {
  std::vector<std::pair<std::string, std::string>> added;
  {
    std::lock_guard lock(mutex);
    for (const log::Category* category : categories) {
      const std::string_view name = category->GetName();
      std::optional<u16> slot = FindLocked(name);
      if (slot && slots[*slot].registered) {
        return Err(ErrorCategory::kConfig, std::format("log category {} is added twice", name));
      }
      if (!slot) {
        if (slots.size() >= log::kMaxCategories) {
          return Err(ErrorCategory::kConfig,
                     std::format("log category {} does not fit, the table is full", name));
        }
        slot = CreateLocked(name, category->GetDescription());
      }
      Slot& entry = slots[*slot];
      entry.registered = true;
      entry.description = std::string(category->GetDescription());
      RecomputeLocked(*slot);
      log::DetailAccess::Slot(*category).store(*slot, std::memory_order_release);
      added.emplace_back(std::string(name), entry.description);
    }
  }
  for (const auto& [name, description] : added) {
    auto entry = std::make_unique<CategoryEntry>(*this, name, description);
    if (auto registered = REX_SETTINGS().Register(*entry); !registered) {
      return registered;
    }
    std::lock_guard lock(mutex);
    if (auto slot = FindLocked(name)) {
      slots[*slot].setting = std::move(entry);
    }
  }
  return Ok();
}

void Logger::State::RemoveCategories(std::span<const log::Category* const> categories) {
  std::vector<std::unique_ptr<SettingOwner>> detached;
  {
    std::lock_guard lock(mutex);
    for (const log::Category* category : categories) {
      auto slot = FindLocked(category->GetName());
      if (!slot || !slots[*slot].registered) {
        continue;
      }
      Slot& entry = slots[*slot];
      entry.registered = false;
      entry.explicit_level.reset();
      RecomputeLocked(*slot);
      detached.push_back(std::move(entry.setting));
    }
  }
  detached.clear();
}

void Logger::State::ApplyDefaultLevel(std::string_view text) {
  auto parsed = log::ParseLevel(text);
  if (!parsed) {
    return;
  }
  std::lock_guard lock(mutex);
  default_level = *parsed;
  RecomputeAllLocked();
}

void Logger::State::ApplyCategoryLevel(std::string_view category, std::string_view text) {
  std::optional<log::Level> parsed_level;
  if (!text.empty()) {
    auto parsed = log::ParseLevel(text);
    if (!parsed) {
      return;
    }
    parsed_level = *parsed;
  }
  std::lock_guard lock(mutex);
  if (auto slot = FindLocked(category)) {
    slots[*slot].explicit_level = parsed_level;
    RecomputeLocked(*slot);
  }
}

bool Logger::State::AddSink(log::ISink& sink) {
  std::lock_guard lock(sinks_mutex);
  if (std::ranges::any_of(consumers,
                          [&sink](const Consumer& entry) { return entry.target == &sink; })) {
    return false;
  }
  auto adapter = std::make_shared<log::ConsumerSink>(sink, dispatch_sequence);
  Replay(*adapter, !file_resolved);
  consumers.push_back(Consumer{&sink, std::move(adapter)});
  RebuildSinksLocked();
  return true;
}

void Logger::State::RemoveSink(log::ISink& sink) {
  std::lock_guard lock(sinks_mutex);
  std::erase_if(consumers, [&sink](const Consumer& entry) { return entry.target == &sink; });
  RebuildSinksLocked();
}

void Logger::State::RebuildSinksLocked() {
  sinks.clear();
  if (debugger) {
    sinks.push_back(debugger);
  }
  if (console) {
    sinks.push_back(console);
  }
  if (file) {
    sinks.push_back(file);
  }
  for (const Consumer& consumer : consumers) {
    sinks.push_back(consumer.adapter);
  }
}

void Logger::State::FlushLocked() {
  for (const auto& sink : sinks) {
    log::SafeFlush(*sink);
  }
}

void Logger::State::FlushSinks() {
  std::lock_guard lock(sinks_mutex);
  const log::InSinks in_sinks;
  FlushLocked();
}

void Logger::State::Flush() {
  engine->flush();
}

void Logger::State::Keep(const log::Message& message) {
  if (!keeping) {
    return;
  }
  kept.push_back(log::Capture(message));
  if (file_resolved) {
    if (kept.size() > log::kReplayLines) {
      kept.pop_front();
    }
  } else if (kept.size() > log::kPreFileLines) {
    kept.pop_front();
    ++discarded;
  }
}

void Logger::State::Replay(spdlog::sinks::sink& sink, bool notice) {
  if (notice && discarded > 0) {
    log::CapturedMessage line;
    line.level = log::Level::kWarn;
    line.category = std::string(log::core.GetName());
    line.text =
        std::format("{} earlier lines were discarded because the log buffer filled", discarded);
    line.time = std::chrono::system_clock::now();
    dispatch_sequence = 0;
    log::SafeLog(sink, log::ToLogMessage(log::View(line)));
  }
  for (const log::CapturedMessage& message : kept) {
    dispatch_sequence = message.sequence;
    log::SafeLog(sink, log::ToLogMessage(log::View(message)));
  }
}

void Logger::State::Dispatch(const spdlog::details::log_msg& message, u64 message_sequence) {
  std::lock_guard lock(sinks_mutex);
  const log::InSinks in_sinks;
  Keep(log::ToMessage(message, message_sequence));
  dispatch_sequence = message_sequence;
  for (const auto& sink : sinks) {
    log::SafeLog(*sink, message);
  }
}

void Logger::State::Write(const log::Message& message) {
  engine->Emit(message);
}

bool Logger::State::LockSinks(std::unique_lock<std::mutex>& lock,
                              std::chrono::milliseconds timeout) {
  if (log::t_in_sinks) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!lock.try_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

bool Logger::State::TryWrite(const log::Category& category, log::Level severity,
                             std::string_view text, std::chrono::milliseconds timeout) {
  const log::Message message{severity,
                             category.GetName(),
                             text,
                             std::chrono::system_clock::now(),
                             platform::GetCallerSystemThreadID(),
                             sequence.fetch_add(1, std::memory_order_relaxed) + 1};
  std::unique_lock lock(sinks_mutex, std::defer_lock);
  if (!LockSinks(lock, timeout)) {
    return false;
  }
  const log::InSinks in_sinks;
  const spdlog::details::log_msg line = log::ToLogMessage(message);
  dispatch_sequence = message.sequence;
  for (const auto& sink : sinks) {
    log::SafeLog(*sink, line);
  }
  return !sinks.empty();
}

bool Logger::State::TryFlush(std::chrono::milliseconds timeout) {
  std::unique_lock lock(sinks_mutex, std::defer_lock);
  if (!LockSinks(lock, timeout)) {
    return false;
  }
  const log::InSinks in_sinks;
  FlushLocked();
  return true;
}

void Logger::State::SetCrashFile(const std::filesystem::path& path) {
  log::CloseCrashFile(
      crash_fd.exchange(path.empty() ? -1 : log::OpenCrashFile(path), std::memory_order_acq_rel));
}

void Logger::State::ApplySettled() {
  ApplyDefaultLevel(owner.level.Get());
  std::string rejection;
  std::string wanted = owner.pattern.Get();
  if (const std::string reason = log::CheckPattern(wanted); !reason.empty()) {
    rejection = std::format("log.pattern '{}' is not used, {}", wanted, reason);
    wanted = owner.pattern.GetDefaultText();
  }
  bool report = false;
  {
    std::lock_guard lock(sinks_mutex);
    if (auto parsed = log::ParseLevel(owner.flush_level.Get())) {
      engine->flush_on(log::ToSpdlog(*parsed));
    }
    pattern = wanted;
    console = owner.console.Get() ? log::MakeConsole(owner.color.Get(), pattern) : nullptr;
    debugger = owner.debugger.Get() ? log::MakeDebugger(pattern) : nullptr;
    keeping = true;
    RebuildSinksLocked();
    report = !rejection.empty() && rejection != rejected_pattern;
    rejected_pattern = rejection;
  }
  if (report) {
    REXLOG_ERROR("{}", rejection);
  }
}

Logger::State& Logger::State::Instance() {
  return *REX_LOGGER().state_;
}

void Logger::State::StopFlusher() {
  spdlog::shutdown();
}

void Logger::State::StartFlusher(std::chrono::seconds interval) {
  spdlog::register_logger(engine);
  spdlog::flush_every(interval);
}

void Logger::State::EndSession() {
  StopFlusher();
  {
    std::lock_guard lock(sinks_mutex);
    file.reset();
    SetCrashFile({});
    kept.clear();
    keeping = true;
    file_resolved = false;
    discarded = 0;
  }
  ApplySettled();
}

Result<void> Logger::State::OpenFile(const std::filesystem::path& log_root,
                                     std::string_view app_name) {
  StopFlusher();
  log::LogFileConfig config;
  config.enabled = owner.file.Get();
  config.folder = log_root;
  config.name = owner.file_name.Get().empty() ? std::string(app_name) : owner.file_name.Get();
  if (const std::filesystem::path named(owner.file_name.Get()); named.is_absolute()) {
    config.path = named;
  }
  config.folder_budget = static_cast<u64>(owner.folder_budget.Get());
  const std::chrono::seconds interval(owner.flush_interval.Get());

  std::string file_error;
  {
    std::lock_guard lock(sinks_mutex);
    file.reset();
    SetCrashFile({});
    if (const auto path = log::ResolveLogFile(config); !path.empty()) {
      try {
        auto opened = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), false);
        opened->set_formatter(log::MakeFormatter(pattern));
        file = std::move(opened);
        SetCrashFile(path);
        Replay(*file, true);
      } catch (const std::exception&) {
        file_error = std::format("log file {} could not be opened", path.string());
      }
    }
    file_resolved = true;
    if (!config.enabled) {
      kept.clear();
      keeping = false;
    }
    while (kept.size() > log::kReplayLines) {
      kept.pop_front();
    }
    RebuildSinksLocked();
  }
  if (interval.count() > 0) {
    StartFlusher(interval);
  }
  if (!file_error.empty()) {
    REXLOG_ERROR("{}", file_error);
    return Err(ErrorCategory::kIO, file_error);
  }
  return Ok();
}

Logger::~Logger() = default;

Logger::Logger() : SettingOwner("log"), state_(std::make_unique<State>(*this)) {
  state_->level_watch =
      level.OnChange([this](const std::string& text) { state_->ApplyDefaultLevel(text); });
  const auto reapply = [this](auto& setting) {
    state_->watches.push_back(setting.OnChange([this](const auto&) { state_->ApplySettled(); }));
  };
  reapply(flush_level);
  reapply(console);
  reapply(debugger);
  reapply(pattern);
  reapply(color);
  (void)REX_SETTINGS().Register(*this);
  const log::Category* core[] = {&log::core, &log::system};
  (void)state_->AddCategories(core);
}

Result<void> Logger::AddCategories(std::span<const log::Category* const> categories) {
  return state_->AddCategories(categories);
}

void Logger::RemoveCategories(std::span<const log::Category* const> categories) {
  state_->RemoveCategories(categories);
}

ScopeGuard Logger::AddSink(log::ISink& sink) {
  if (!state_->AddSink(sink)) {
    return ScopeGuard();
  }
  State* state = state_.get();
  return ScopeGuard([state, &sink] { state->RemoveSink(sink); });
}

void Logger::Flush() {
  state_->Flush();
}

void Logger::Emit(const log::Message& message) {
  state_->Write(message);
}

void Logger::ApplySettled() {
  state_->ApplySettled();
}

void Logger::EndSession() {
  state_->EndSession();
}

Result<void> Logger::OpenFile(const std::filesystem::path& log_root, std::string_view app_name) {
  return state_->OpenFile(log_root, app_name);
}

Logger& GetLogger() {
  static Logger* logger = new Logger();
  return *logger;
}

}  // namespace rex

namespace rex::log {

Logger::State& GetLoggerInstance() {
  return Logger::State::Instance();
}

}  // namespace rex::log
