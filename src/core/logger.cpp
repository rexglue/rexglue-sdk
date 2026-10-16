#include "core/logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <format>
#include <ranges>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging/log.h>
#include <rex/platform/thread.h>
#include <rex/string/numeric.h>

namespace rex::log {

class DetailAccess {
 public:
  static std::atomic<u16>& Slot(const Category& category) { return category.slot_; }
};

namespace {

thread_local bool t_in_sinks = false;

struct InSinks {
  InSinks() { t_in_sinks = true; }
  ~InSinks() { t_in_sinks = false; }
};

constexpr std::array<std::string_view, 8> kLevelNames = {"trace", "dev",   "debug",    "info",
                                                         "warn",  "error", "critical", "off"};

constexpr std::array<std::string_view, 9> kCategoryValues = {
    "", "trace", "dev", "debug", "info", "warn", "error", "critical", "off"};

constexpr std::array<std::string_view, 3> kReservedNames = {"level", "file", "flush_interval"};

std::string Lowercase(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

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

}  // namespace

[[nodiscard]] Result<Level> ParseLevel(std::string_view text) {
  const std::string lower = Lowercase(text);
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
  Logger& logger = GetLoggerInstance();
  u16 slot = slot_.load(std::memory_order_acquire);
  if (slot == kUnresolved) {
    slot = logger.Resolve(*this);
  }
  return level != Level::kOff && level >= logger.SlotLevel(slot);
}

namespace detail {

void Write(const Category& category, Level level, std::string_view text) {
  GetLoggerInstance().Write(category, level, text);
}

void WriteFailure(const Category& category, const char* what) noexcept {
  try {
    Write(category, Level::kError, std::format("log message dropped, {}", what));
  } catch (...) {
    std::fprintf(stderr, "log message dropped, %s\n", what);
  }
}

void WriteStub(std::atomic_flag& seen, std::string_view name, std::string_view detail) {
  const Level level = seen.test_and_set(std::memory_order_relaxed) ? Level::kDev : Level::kWarn;
  if (!kernel.IsEnabled(level)) {
    return;
  }
  Write(kernel, level,
        detail.empty() ? std::format("{} stub", name) : std::format("{} stub, {}", name, detail));
}

}  // namespace detail

void Flush() {
  GetLoggerInstance().Flush();
}

Result<void> Start(const LogConfig& config) {
  return GetLoggerInstance().StartStandalone(config);
}

void Stop() {
  GetLoggerInstance().StopStandalone();
}

Logger::Logger()
    : bootstrap_debugger_(std::make_unique<DebuggerSink>(LineFormat{})),
      start_time_(std::chrono::system_clock::now()) {
  slots_.reserve(kMaxCategories);
  for (auto& level : levels_) {
    level.store(Level::kInfo, std::memory_order_relaxed);
  }
  CreateLocked(core.GetName(), core.GetDescription());
  sinks_.push_back(bootstrap_debugger_.get());
}

std::optional<u16> Logger::FindLocked(std::string_view name) const {
  for (size_t index = 0; index < slots_.size(); ++index) {
    if (slots_[index].name == name) {
      return static_cast<u16>(index);
    }
  }
  return std::nullopt;
}

u16 Logger::CreateLocked(std::string_view name, std::string_view description) {
  if (slots_.size() >= kMaxCategories) {
    return 0;
  }
  Slot slot;
  slot.name = std::string(name);
  slot.description = std::string(description);
  slots_.push_back(std::move(slot));
  const auto index = static_cast<u16>(slots_.size() - 1);
  RecomputeLocked(index);
  return index;
}

void Logger::RecomputeLocked(u16 slot) {
  const Level level = slots_[slot].explicit_level.value_or(default_level_);
  levels_[slot].store(level, std::memory_order_release);
}

void Logger::RecomputeAllLocked() {
  for (size_t index = 0; index < slots_.size(); ++index) {
    RecomputeLocked(static_cast<u16>(index));
  }
}

u16 Logger::Resolve(const Category& category) {
  bool created = false;
  u16 slot = 0;
  {
    std::lock_guard lock(mutex_);
    if (auto found = FindLocked(category.GetName())) {
      slot = *found;
    } else {
      slot = CreateLocked(category.GetName(), category.GetDescription());
      created = true;
    }
  }
  DetailAccess::Slot(category).store(slot, std::memory_order_release);
  if (created) {
    REXLOG_WARN(core, "log category {} is not registered", category.GetName());
  }
  return slot;
}

Result<void> Logger::Register(std::span<const Category* const> categories) {
  std::vector<u16> added;
  {
    std::lock_guard lock(mutex_);
    for (const Category* category : categories) {
      const std::string_view name = category->GetName();
      if (std::ranges::contains(kReservedNames, name)) {
        return Err(ErrorCategory::kConfig,
                   std::format("log category {} collides with the log.{} cvar", name, name));
      }
      std::optional<u16> slot = FindLocked(name);
      if (slot && slots_[*slot].registered) {
        return Err(ErrorCategory::kConfig,
                   std::format("log category {} is registered twice", name));
      }
      if (!slot) {
        if (slots_.size() >= kMaxCategories) {
          return Err(ErrorCategory::kConfig,
                     std::format("log category {} does not fit, the table is full", name));
        }
        slot = CreateLocked(name, category->GetDescription());
      }
      Slot& entry = slots_[*slot];
      entry.registered = true;
      entry.description = std::string(category->GetDescription());
      if (auto level = config_levels_.find(name); level != config_levels_.end()) {
        entry.explicit_level = level->second;
      }
      RecomputeLocked(*slot);
      DetailAccess::Slot(*category).store(*slot, std::memory_order_release);
      added.push_back(*slot);
    }
  }
  for (u16 slot : added) {
    AttachCvar(slot);
  }
  return Ok();
}

void Logger::Unregister(std::span<const Category* const> categories) {
  std::vector<Slot> detached;
  {
    std::lock_guard lock(mutex_);
    for (const Category* category : categories) {
      auto slot = FindLocked(category->GetName());
      if (!slot || !slots_[*slot].registered) {
        continue;
      }
      Slot& entry = slots_[*slot];
      entry.registered = false;
      entry.explicit_level.reset();
      RecomputeLocked(*slot);
      Slot moved;
      moved.cvar = std::move(entry.cvar);
      moved.callback = std::move(entry.callback);
      detached.push_back(std::move(moved));
    }
  }
  for (Slot& slot : detached) {
    DetachCvar(slot);
  }
}

void Logger::AttachCvar(u16 slot) {
  if (!cvars_) {
    return;
  }
  cvar::Var<std::string>* var = nullptr;
  std::string cvar_name;
  {
    std::lock_guard lock(mutex_);
    Slot& entry = slots_[slot];
    entry.cvar_name = std::format("log.{}", entry.name);
    const std::string_view initial =
        entry.explicit_level ? LevelName(*entry.explicit_level) : std::string_view{};
    entry.cvar = std::make_unique<cvar::Var<std::string>>(
        cvar::Var<std::string>(entry.cvar_name, initial, entry.description)
            .SetAllowed(kCategoryValues));
    var = entry.cvar.get();
    cvar_name = entry.cvar_name;
  }
  const std::array<const cvar::IVar*, 1> vars = {var};
  if (auto added = cvars_->Add(vars); !added) {
    REXLOG_ERROR(core, "{}", added.error().message);
    return;
  }
  std::string name = cvar_name.substr(4);
  if (auto value = cvars_->Get(cvar_name); value && !value->empty()) {
    ApplyCvarLevel(name, *value);
  }
  auto handle = cvars_->OnChange(cvar_name, [this, name](std::string_view, std::string_view value) {
    ApplyCvarLevel(name, value);
  });
  if (handle) {
    std::lock_guard lock(mutex_);
    slots_[slot].callback = std::move(*handle);
  }
}

void Logger::DetachCvar(Slot& slot) {
  slot.callback.Reset();
  if (slot.cvar && cvars_) {
    const std::array<const cvar::IVar*, 1> vars = {slot.cvar.get()};
    cvars_->Unregister(vars);
  }
  slot.cvar.reset();
}

Result<void> Logger::ApplyLevel(std::string_view category, std::optional<Level> level) {
  std::lock_guard lock(mutex_);
  if (category.empty()) {
    if (!level) {
      return Err(ErrorCategory::kConfig, "the default log level cannot be empty");
    }
    default_level_ = *level;
    RecomputeAllLocked();
    return Ok();
  }
  auto slot = FindLocked(category);
  if (!slot) {
    return Err(ErrorCategory::kNotFound, std::format("unknown log category {}", category));
  }
  slots_[*slot].explicit_level = level;
  RecomputeLocked(*slot);
  return Ok();
}

void Logger::ApplyCvarLevel(std::string_view category, std::string_view value) {
  std::optional<Level> level;
  if (!value.empty()) {
    auto parsed = ParseLevel(value);
    if (!parsed) {
      REXLOG_WARN(core, "{}", parsed.error().message);
      return;
    }
    level = *parsed;
  }
  if (auto applied = ApplyLevel(category, level); !applied) {
    REXLOG_WARN(core, "{}", applied.error().message);
  }
}

Result<void> Logger::SetLevel(std::string_view category, std::optional<Level> level) {
  REX_TRY_VOID(ApplyLevel(category, level));
  if (cvars_) {
    const std::string name = category.empty() ? "log.level" : std::format("log.{}", category);
    if (cvars_->Find(name)) {
      REX_TRY_VOID(cvars_->Set(name, level ? LevelName(*level) : std::string_view{}));
    }
  }
  return Ok();
}

Result<Level> Logger::GetLevel(std::string_view category) const {
  std::lock_guard lock(mutex_);
  if (category.empty()) {
    return default_level_;
  }
  auto slot = FindLocked(category);
  if (!slot) {
    return Err<Level>(ErrorCategory::kNotFound, std::format("unknown log category {}", category));
  }
  return levels_[*slot].load(std::memory_order_relaxed);
}

std::vector<CategoryInfo> Logger::GetCategories() const {
  std::lock_guard lock(mutex_);
  std::vector<CategoryInfo> out;
  out.reserve(slots_.size());
  for (size_t index = 0; index < slots_.size(); ++index) {
    const auto& slot = slots_[index];
    out.push_back(CategoryInfo{slot.name, slot.description,
                               levels_[index].load(std::memory_order_relaxed), slot.explicit_level,
                               slot.registered});
  }
  return out;
}

void Logger::AddSink(ISink& sink) {
  std::lock_guard lock(sinks_mutex_);
  if (std::ranges::contains(consumer_sinks_, &sink)) {
    return;
  }
  consumer_sinks_.push_back(&sink);
  RebuildSinksLocked();
}

void Logger::RemoveSink(ISink& sink) {
  std::lock_guard lock(sinks_mutex_);
  std::erase(consumer_sinks_, &sink);
  RebuildSinksLocked();
}

void Logger::RebuildSinksLocked() {
  sinks_.clear();
  if (!started_) {
    sinks_.push_back(bootstrap_debugger_.get());
  } else {
    if (debugger_) {
      sinks_.push_back(debugger_.get());
    }
    if (console_) {
      sinks_.push_back(console_.get());
    }
    if (file_) {
      sinks_.push_back(file_.get());
    }
    sinks_.insert(sinks_.end(), config_sinks_.begin(), config_sinks_.end());
  }
  for (ISink* sink : consumer_sinks_) {
    if (!std::ranges::contains(sinks_, sink)) {
      sinks_.push_back(sink);
    }
  }
}

void Logger::SetFormat(const LineFormat& format) {
  std::lock_guard lock(sinks_mutex_);
  if (debugger_) {
    debugger_->SetFormat(format);
  }
  if (console_) {
    console_->SetFormat(format);
  }
}

void Logger::FlushLocked() {
  for (ISink* sink : sinks_) {
    sink->Flush();
  }
}

void Logger::Flush() {
  std::lock_guard lock(sinks_mutex_);
  const InSinks in_sinks;
  FlushLocked();
}

std::filesystem::path Logger::GetFilePath() const {
  std::lock_guard lock(sinks_mutex_);
  return file_ ? file_->GetPath() : std::filesystem::path{};
}

void Logger::Write(const Category& category, Level level, std::string_view text) {
  const Message message{level,
                        category.GetName(),
                        text,
                        std::chrono::system_clock::now(),
                        platform::CurrentThreadSystemId(),
                        sequence_.fetch_add(1, std::memory_order_relaxed) + 1};
  std::lock_guard lock(sinks_mutex_);
  const InSinks in_sinks;
  if (buffering_) {
    if (bootstrap_.size() == kBootstrapMessages) {
      bootstrap_.pop_front();
    }
    bootstrap_.push_back(Capture(message));
  }
  for (ISink* sink : sinks_) {
    sink->Write(message);
  }
  if (level >= flush_level_) {
    FlushLocked();
  }
}

bool Logger::LockSinks(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout) {
  if (t_in_sinks) {
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

bool Logger::TryWrite(const Category& category, Level level, std::string_view text,
                      std::chrono::milliseconds timeout) {
  const Message message{level,
                        category.GetName(),
                        text,
                        std::chrono::system_clock::now(),
                        platform::CurrentThreadSystemId(),
                        sequence_.fetch_add(1, std::memory_order_relaxed) + 1};
  std::unique_lock lock(sinks_mutex_, std::defer_lock);
  if (!LockSinks(lock, timeout)) {
    return false;
  }
  const InSinks in_sinks;
  if (!started_) {
    return false;
  }
  for (ISink* sink : sinks_) {
    sink->Write(message);
  }
  return !sinks_.empty();
}

bool Logger::TryFlush(std::chrono::milliseconds timeout) {
  std::unique_lock lock(sinks_mutex_, std::defer_lock);
  if (!LockSinks(lock, timeout)) {
    return false;
  }
  const InSinks in_sinks;
  FlushLocked();
  return true;
}

void Logger::OpenSinks(const LogConfig& config) {
  std::lock_guard lock(sinks_mutex_);
  debugger_ = config.debugger ? std::make_unique<DebuggerSink>(config.format) : nullptr;
  console_ = config.console ? std::make_unique<ConsoleSink>(config.format) : nullptr;
  file_.reset();
  SetCrashFile({});
  config_sinks_ = config.sinks;
  flush_level_ = config.flush_level;
  started_ = true;
  RebuildSinksLocked();
  for (const CapturedMessage& message : bootstrap_) {
    const Message view = View(message);
    for (ISink* sink : sinks_) {
      if (sink != debugger_.get() && !std::ranges::contains(consumer_sinks_, sink)) {
        sink->Write(view);
      }
    }
  }
}

void Logger::ApplyConfigLevels(const LogConfig& config) {
  std::lock_guard lock(mutex_);
  config_levels_ = config.levels;
  default_level_ = config.level;
  for (Slot& slot : slots_) {
    if (auto level = config_levels_.find(slot.name); level != config_levels_.end()) {
      slot.explicit_level = level->second;
    }
  }
  RecomputeAllLocked();
}

void Logger::HookCvars(cvar::ICvarRegistry& cvars) {
  cvars_ = &cvars;
  std::vector<u16> unattached;
  Level level = Level::kInfo;
  {
    std::lock_guard lock(mutex_);
    for (size_t index = 0; index < slots_.size(); ++index) {
      if (const auto& slot = slots_[index]; slot.registered && !slot.cvar) {
        unattached.push_back(static_cast<u16>(index));
      }
    }
    level = default_level_;
  }
  for (u16 slot : unattached) {
    AttachCvar(slot);
  }
  if (auto set = cvars.SetDefault("log.level", LevelName(level)); !set) {
    REXLOG_ERROR(core, "{}", set.error().message);
  }
  if (auto handle = cvars.OnChange("log.level", [this](std::string_view, std::string_view value) {
        ApplyCvarLevel("", value);
      })) {
    level_callback_ = std::move(*handle);
  }
}

void Logger::UnhookCvars() {
  level_callback_.Reset();
  std::vector<Slot> detached;
  {
    std::lock_guard lock(mutex_);
    for (Slot& slot : slots_) {
      if (slot.cvar) {
        Slot moved;
        moved.cvar = std::move(slot.cvar);
        moved.callback = std::move(slot.callback);
        detached.push_back(std::move(moved));
      }
    }
  }
  for (Slot& slot : detached) {
    DetachCvar(slot);
  }
  cvars_ = nullptr;
}

void Logger::SetCrashFile(const std::filesystem::path& path) {
  CloseCrashFile(
      crash_fd_.exchange(path.empty() ? -1 : OpenCrashFile(path), std::memory_order_acq_rel));
}

void Logger::OpenFile(const LogFileConfig& file, std::chrono::seconds interval) {
  std::string file_error;
  {
    std::lock_guard lock(sinks_mutex_);
    if (const auto path = ResolveLogFile(file); !path.empty()) {
      if (auto opened = FileSink::Open(path, file.format)) {
        file_ = std::move(*opened);
        SetCrashFile(path);
        for (const CapturedMessage& message : bootstrap_) {
          file_->Write(View(message));
        }
      } else {
        file_error = opened.error().message;
      }
    }
    bootstrap_.clear();
    buffering_ = false;
    RebuildSinksLocked();
  }
  if (interval.count() > 0) {
    flush_thread_ = platform::Spawn("Log Flush", [this, interval](const std::stop_token& stop) {
      RunFlushThread(stop, interval);
    });
  }
  if (!file_error.empty()) {
    REXLOG_ERROR(core, "{}", file_error);
  }
}

Result<void> Logger::RegisterStarted(const LogConfig& config) {
  started_categories_.clear();
  started_categories_.push_back(&core);
  started_categories_.insert(started_categories_.end(), config.categories.begin(),
                             config.categories.end());
  return Register(started_categories_);
}

Result<void> Logger::Start(const LogConfig& config, cvar::ICvarRegistry& cvars) {
  if (standalone_) {
    HookCvars(cvars);
    return Ok();
  }
  OpenSinks(config);
  file_config_ = config.file;
  flush_interval_ = config.flush_interval;
  ApplyConfigLevels(config);
  if (auto registered = RegisterStarted(config); !registered) {
    Stop();
    return registered;
  }
  HookCvars(cvars);
  return Ok();
}

void Logger::Finish(cvar::ICvarRegistry& cvars) {
  if (auto value = cvars.Get("log.level")) {
    ApplyCvarLevel("", *value);
  }
  if (standalone_) {
    return;
  }
  LogFileConfig file = file_config_;
  if (auto path = cvars.Get("log.file"); path && !path->empty()) {
    file.path = *path;
    file.enabled = true;
  }
  std::chrono::seconds interval = flush_interval_;
  if (cvars.Source("log.flush_interval") != cvar::CvarSource::kDefault) {
    if (auto seconds = cvars.Get("log.flush_interval")) {
      interval = std::chrono::seconds(std::max<i32>(from_string<i32>(*seconds), 0));
    }
  }
  OpenFile(file, interval);
}

Result<void> Logger::StartStandalone(const LogConfig& config) {
  if (started_) {
    return Err(ErrorCategory::kRuntime, "the logger is already started");
  }
  OpenSinks(config);
  ApplyConfigLevels(config);
  if (auto registered = RegisterStarted(config); !registered) {
    Stop();
    return registered;
  }
  OpenFile(config.file, config.flush_interval);
  standalone_ = true;
  return Ok();
}

void Logger::RunFlushThread(const std::stop_token& stop, std::chrono::seconds interval) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(flush_mutex_);
      flush_wake_.wait_for(lock, stop, interval, [] { return false; });
    }
    if (!stop.stop_requested()) {
      Flush();
    }
  }
}

void Logger::Stop() {
  UnhookCvars();
  if (standalone_) {
    return;
  }
  StopAll();
}

void Logger::DetachCvars() {
  UnhookCvars();
}

void Logger::StopStandalone() {
  UnhookCvars();
  standalone_ = false;
  StopAll();
}

void Logger::StopAll() {
  if (flush_thread_.joinable()) {
    flush_thread_.request_stop();
    flush_thread_.join();
  }
  if (!started_categories_.empty()) {
    Unregister(started_categories_);
    started_categories_.clear();
  }
  std::lock_guard lock(sinks_mutex_);
  FlushLocked();
  started_ = false;
  buffering_ = true;
  debugger_.reset();
  console_.reset();
  file_.reset();
  SetCrashFile({});
  config_sinks_.clear();
  RebuildSinksLocked();
}

Logger& GetLoggerInstance() {
  static Logger instance;
  return instance;
}

void* GetLoggerInterface() {
  return static_cast<ILogger*>(&GetLoggerInstance());
}

}  // namespace rex::log

namespace rex::detail {

Result<void> StartLogger(const LogConfig& config, cvar::ICvarRegistry& cvars) {
  return log::GetLoggerInstance().Start(config, cvars);
}

void FinishLoggerStart(cvar::ICvarRegistry& cvars) {
  log::GetLoggerInstance().Finish(cvars);
}

void DetachLoggerCvars() {
  log::GetLoggerInstance().DetachCvars();
}

void StopLogger() {
  log::GetLoggerInstance().Stop();
}

}  // namespace rex::detail
