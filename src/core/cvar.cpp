/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <CLI/CLI.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform/env.h>

#include <toml++/toml.hpp>

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Set once cvar::Init has parsed the command line; later registrations are
// from runtime-loaded modules and drain pending values.
std::atomic<bool> g_init_done = false;

// Recursive: FlagRegistrar chain methods re-enter; change callbacks invoked
// from SetFlagByName must not mutate the registry.
std::recursive_mutex& GetRegistryMutex() {
  static std::recursive_mutex m;
  return m;
}

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
  static std::vector<FlagEntry> registry;
  return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
  static std::unordered_map<std::string, size_t> index;
  return index;
}

// Values that arrived before their cvar was registered; runtime-loaded
// modules register cvars long after Init/LoadConfig.
struct PendingValues {
  std::optional<std::string> cmdline;
  std::optional<std::string> config;
};

std::unordered_map<std::string, PendingValues>& GetPendingValuesStorage() {
  static std::unordered_map<std::string, PendingValues> pending;
  return pending;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
  std::string result = "REX_";
  for (char c : name) {
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

enum class ApplyResult { kApplied, kSkipped, kRejected };

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source);

bool Outranks(Source source, const FlagEntry& entry) {
  return source >= entry.source;
}

// Unvalidated apply, for the command line and environment paths.
bool ApplyFromSource(FlagEntry& entry, std::string_view value, Source source) {
  if (!Outranks(source, entry) || !entry.setter(value)) {
    return false;
  }
  entry.source = source;
  return true;
}

// Recursively apply TOML values
void ApplyTomlTable(const toml::table& table, const std::string& prefix) {
  for (const auto& [key, value] : table) {
    std::string full_key = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);

    if (value.is_table()) {
      ApplyTomlTable(*value.as_table(), full_key);
    } else {
      std::string value_str;
      if (value.is_boolean()) {
        value_str = value.as_boolean()->get() ? "true" : "false";
      } else if (value.is_integer()) {
        value_str = std::to_string(value.as_integer()->get());
      } else if (value.is_floating_point()) {
        value_str = std::to_string(value.as_floating_point()->get());
      } else if (value.is_string()) {
        value_str = value.as_string()->get();
      } else {
        REXLOG_WARN("Config: unsupported type for key '{}'", full_key);
        continue;
      }

      if (GetFlagInfo(full_key) == nullptr) {
        std::lock_guard lock(GetRegistryMutex());
        GetPendingValuesStorage()[full_key].config = value_str;
        REXLOG_DEBUG("Config: '{}' deferred (cvar not yet registered)", full_key);
        continue;
      }

      switch (SetFlagFromSource(full_key, value_str, Source::kConfig)) {
        case ApplyResult::kApplied:
          REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
          break;
        case ApplyResult::kSkipped:
          REXLOG_DEBUG("Config: {} ignored, already set by a higher-priority source", full_key);
          break;
        case ApplyResult::kRejected:
          REXLOG_WARN("Config: invalid value for cvar '{}'", full_key);
          break;
      }
    }
  }
}

// todo(tomc): move restart manager to Runtime
std::vector<std::string>& GetPendingRestartStorage() {
  static std::vector<std::string> pending;
  return pending;
}

// Callback storage for change notifications
std::unordered_map<std::string, std::vector<ChangeCallback>>& GetCallbackStorage() {
  static std::unordered_map<std::string, std::vector<ChangeCallback>> callbacks;
  return callbacks;
}

void MarkPendingRestart(std::string_view name) {
  auto& pending = GetPendingRestartStorage();
  std::string name_str(name);
  if (std::find(pending.begin(), pending.end(), name_str) == pending.end()) {
    pending.push_back(name_str);
  }
}

bool ValidateConstraints(const FlagEntry& entry, std::string_view value) {
  const auto& c = entry.constraints;

  // Range validation for numeric types
  if (c.HasRangeConstraint()) {
    double numeric_val = 0;
    if (entry.type == FlagType::String || entry.type == FlagType::Boolean) {
      // These types don't have numeric range constraints
    } else if (entry.type == FlagType::Double) {
      if (!ParseDouble(value, numeric_val))
        return false;
    } else {
      // Integer types
      int64_t int_val = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
      if (ec != std::errc())
        return false;
      numeric_val = static_cast<double>(int_val);
    }

    if (c.min.has_value() && numeric_val < *c.min) {
      REXLOG_WARN("Flag '{}': value {} below min ({})", entry.name, value, *c.min);
      return false;
    }
    if (c.max.has_value() && numeric_val > *c.max) {
      REXLOG_WARN("Flag '{}': value {} exceeds max ({})", entry.name, value, *c.max);
      return false;
    }
  }

  // Allowed values validation
  if (c.HasAllowedValues()) {
    bool found = false;
    for (const auto& allowed : c.allowed_values) {
      if (allowed == value) {
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Flag '{}': '{}' not in allowed values", entry.name, value);
      return false;
    }
  }

  // Custom validator
  if (c.custom_validator && !c.custom_validator(value)) {
    REXLOG_WARN("Flag '{}': custom validation failed for '{}'", entry.name, value);
    return false;
  }

  return true;
}

}  // namespace

//=============================================================================
// Registry
//=============================================================================

std::vector<FlagEntry>& GetRegistry() {
  return GetRegistryStorage();
}

std::optional<size_t> RegisterFlag(FlagEntry entry) {
  std::lock_guard lock(GetRegistryMutex());
  (void)GetCallbackStorage();
  (void)GetPendingRestartStorage();
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  auto it = index.find(entry.name);
  if (it != index.end()) {
    REXLOG_ERROR("cvar: duplicate registration of '{}'; second registration ignored", entry.name);
    return std::nullopt;
  }
  size_t pos = storage.size();
  index[entry.name] = pos;
  storage.push_back(std::move(entry));

  // Late registration: replay pending values in ascending priority.
  if (g_init_done) {
    FlagEntry& stored = storage[pos];
    auto& pending = GetPendingValuesStorage();
    auto pending_it = pending.find(stored.name);
    if (pending_it != pending.end() && pending_it->second.config) {
      ApplyFromSource(stored, *pending_it->second.config, Source::kConfig);
    }
    auto env_value = rex::platform::env::get(FlagNameToEnvVar(stored.name));
    if (env_value.has_value()) {
      ApplyFromSource(stored, *env_value, Source::kEnvironment);
    }
    if (pending_it != pending.end()) {
      if (pending_it->second.cmdline) {
        ApplyFromSource(stored, *pending_it->second.cmdline, Source::kCommandLine);
      }
      pending.erase(pending_it);
    }
  }
  return pos;
}

void UnregisterFlag(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto& storage = GetRegistryStorage();
  std::string key(name);
  auto idx_it = index.find(key);
  if (idx_it == index.end()) {
    return;
  }
  size_t pos = idx_it->second;
  index.erase(idx_it);
  storage.erase(storage.begin() + pos);
  for (auto& [n, i] : index) {
    if (i > pos) {
      --i;
    }
  }
  GetCallbackStorage().erase(key);
  auto& pending = GetPendingRestartStorage();
  pending.erase(std::remove(pending.begin(), pending.end(), key), pending.end());
}

void FlagRegistrar::apply_(std::function<void(FlagEntry&)> fn) {
  if (owned_name_.empty()) {
    return;
  }
  std::lock_guard lock(GetRegistryMutex());
  auto& index = GetRegistryIndex();
  auto it = index.find(owned_name_);
  if (it == index.end()) {
    return;
  }
  fn(GetRegistryStorage()[it->second]);
}

namespace {

ApplyResult SetFlagFromSource(std::string_view name, std::string_view value, Source source) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return ApplyResult::kRejected;
  }

  auto& entry = GetRegistryStorage()[it->second];

  if (!Outranks(source, entry)) {
    return ApplyResult::kSkipped;
  }

  if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
    REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
    return ApplyResult::kRejected;
  }

  if (!ValidateConstraints(entry, value)) {
    return ApplyResult::kRejected;
  }

  if (!entry.setter(value)) {
    return ApplyResult::kRejected;
  }
  entry.source = source;

  if (entry.lifecycle == Lifecycle::kRequiresRestart) {
    MarkPendingRestart(name);
  }

  auto& callbacks = GetCallbackStorage();
  auto callback_it = callbacks.find(std::string(name));
  if (callback_it != callbacks.end()) {
    for (const auto& callback : callback_it->second) {
      callback(name, value);
    }
  }

  return ApplyResult::kApplied;
}

}  // namespace

bool SetFlagByName(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kRuntime) == ApplyResult::kApplied;
}

bool SetFlagFromCommandLine(std::string_view name, std::string_view value) {
  return SetFlagFromSource(name, value, Source::kCommandLine) != ApplyResult::kRejected;
}

bool InvokeCommand(std::string_view name, std::string_view args) {
  std::function<void(std::string_view)> cb;
  {
    std::lock_guard lock(GetRegistryMutex());
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
      return false;
    }
    const auto& entry = GetRegistryStorage()[it->second];
    if (entry.type != FlagType::Command) {
      return false;
    }
    // Copy the callback out from under the lock; GetFlagInfo pointers are
    // invalidated by registry mutation and a command may touch the registry.
    cb = entry.command_callback;
  }
  if (!cb) {
    return false;
  }
  cb(args);
  return true;
}

std::string GetFlagByName(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return "";
  }

  return GetRegistryStorage()[it->second].getter();
}

Source GetFlagSource(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return Source::kDefault;
  }
  return GetRegistryStorage()[it->second].source;
}

std::vector<std::string> ListFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  result.reserve(GetRegistryStorage().size());
  for (const auto& entry : GetRegistryStorage()) {
    result.push_back(entry.name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc) {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.lifecycle == lc) {
      result.push_back(entry.name);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  // Pointer is invalidated by any subsequent registry call.
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return nullptr;
  }
  return &GetRegistryStorage()[it->second];
}

template <>
bool Query<bool>(std::string_view name) {
  std::string v = GetFlagByName(name);
  return rex::string::from_string<bool>(v, false);
}

template <>
int32_t Query<int32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
int64_t Query<int64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  int64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint32_t Query<uint32_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint32_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
uint64_t Query<uint64_t>(std::string_view name) {
  std::string v = GetFlagByName(name);
  uint64_t out = 0;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

template <>
double Query<double>(std::string_view name) {
  std::string v = GetFlagByName(name);
  double out = 0.0;
  ParseDouble(v, out);
  return out;
}

template <>
std::string Query<std::string>(std::string_view name) {
  return GetFlagByName(name);
}

std::vector<std::string> GetPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
  std::lock_guard lock(GetRegistryMutex());
  GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return;
  }
  auto& entry = GetRegistryStorage()[it->second];
  entry.setter(entry.default_value);
  entry.source = Source::kDefault;
}

void ResetAllToDefaults() {
  std::lock_guard lock(GetRegistryMutex());
  for (auto& entry : GetRegistryStorage()) {
    entry.setter(entry.default_value);
    entry.source = Source::kDefault;
  }
}

bool HasNonDefaultValue(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  auto it = GetRegistryIndex().find(std::string(name));
  if (it == GetRegistryIndex().end()) {
    return false;
  }
  const auto& entry = GetRegistryStorage()[it->second];
  return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
  std::lock_guard lock(GetRegistryMutex());
  std::vector<std::string> result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      result.push_back(entry.name);
    }
  }
  return result;
}

// A TOML basic string: quotes, backslashes and control characters escaped,
// so a value (a Windows path, text typed in a setting) can't break the file
// or add lines of its own to it.
static std::string TomlString(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          constexpr char kHex[] = "0123456789ABCDEF";
          const auto u = static_cast<unsigned char>(c);
          out += "\\u00";
          out += kHex[u >> 4];
          out += kHex[u & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string SerializeToTOML() {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.getter() != entry.default_value) {
      if (entry.type == FlagType::String) {
        result += entry.name + " = " + TomlString(entry.getter()) + "\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

std::string SerializeToTOML(std::string_view category) {
  std::lock_guard lock(GetRegistryMutex());
  std::string result;
  for (const auto& entry : GetRegistryStorage()) {
    if (entry.category == category && entry.getter() != entry.default_value) {
      if (entry.type == FlagType::String) {
        result += entry.name + " = " + TomlString(entry.getter()) + "\n";
      } else {
        result += entry.name + " = " + entry.getter() + "\n";
      }
    }
  }
  return result;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
  std::lock_guard lock(GetRegistryMutex());
  GetCallbackStorage().erase(std::string(name));
}

//=============================================================================
// Initialization
//=============================================================================

std::vector<std::string> Init(int argc, char** argv) {
  CLI::App app{"", ""};
  app.allow_extras();

  for (auto& entry : GetRegistryStorage()) {
    if (entry.type == FlagType::Boolean) {
      app.add_flag_function(
          "--" + entry.name + ",!--no-" + entry.name,
          [&entry](int64_t count) {
            ApplyFromSource(entry, count > 0 ? "true" : "false", Source::kCommandLine);
          },
          entry.description);
    } else {
      app.add_option_function<std::string>(
          "--" + entry.name,
          [&entry](const std::string& val) { ApplyFromSource(entry, val, Source::kCommandLine); },
          entry.description);
    }
  }

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // TODO(tomc): dumb workaround for the stupid chicken and its egg.
    //             dont call rex logging funcs here for now.
    fprintf(stderr, "cvar: CLI11  parse error: %s\n", e.what());
  }

  // Stash unrecognized --options for cvars that register later. Supported
  // forms: --name=value, --name (true), --no-name (false); a separated
  // "--name value" pair is ambiguous with a positional, so never consumed.
  std::vector<std::string> positional;
  for (const auto& arg : app.remaining()) {
    std::string_view view(arg);
    if (!view.starts_with("--")) {
      positional.push_back(arg);
      continue;
    }
    view.remove_prefix(2);
    std::string name;
    std::string value = "true";
    if (auto eq = view.find('='); eq != std::string_view::npos) {
      name.assign(view.substr(0, eq));
      value.assign(view.substr(eq + 1));
    } else if (view.starts_with("no-")) {
      name.assign(view.substr(3));
      value = "false";
    } else {
      name.assign(view);
    }
    std::lock_guard lock(GetRegistryMutex());
    GetPendingValuesStorage()[name].cmdline = std::move(value);
  }
  g_init_done = true;
  return positional;
}

void LoadConfig(const std::filesystem::path& config_path) {
  if (!std::filesystem::exists(config_path)) {
    REXLOG_DEBUG("Config file not found: {}", config_path.string());
    return;
  }

  try {
    auto config = toml::parse_file(config_path.string());
    ApplyTomlTable(config, "");
    REXLOG_DEBUG("Loaded config from {}", config_path.string());
  } catch (const toml::parse_error& err) {
    REXLOG_ERROR("Failed to parse config {}: {}", config_path.string(), err.what());
  }
}

void ApplyEnvironment() {
  int count = 0;
  for (auto& entry : GetRegistryStorage()) {
    std::string env_name = FlagNameToEnvVar(entry.name);
    auto env_value = rex::platform::env::get(env_name);
    if (!env_value.has_value()) {
      continue;
    }
    if (!Outranks(Source::kEnvironment, entry)) {
      REXLOG_DEBUG("Env: {} ignored, already set on the command line", entry.name);
      continue;
    }
    if (ApplyFromSource(entry, *env_value, Source::kEnvironment)) {
      REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, *env_value, env_name);
      ++count;
    } else {
      REXLOG_WARN("Env: failed to parse {} = {}", env_name, *env_value);
    }
  }

  if (count > 0) {
    REXLOG_INFO("Applied {} environment variable override(s)", count);
  }
}

void FinalizeInit() {
  std::lock_guard lock(g_mutex);
  g_finalized = true;
  for (const auto& [name, values] : GetPendingValuesStorage()) {
    (void)values;
    REXLOG_WARN("Config: unknown cvar '{}'", name);
  }
  REXLOG_DEBUG("cvar: initialization finalized");
}

bool IsFinalized() {
  return g_finalized;
}

void SaveConfig(const std::filesystem::path& config_path) {
  std::string content = SerializeToTOML();
  if (content.empty()) {
    REXLOG_DEBUG("SaveConfig: no modified flags to save");
    return;
  }

  try {
    std::ofstream file(config_path);
    if (!file) {
      REXLOG_ERROR("SaveConfig: failed to open {}", config_path.string());
      return;
    }
    file << "# Auto-generated cvar configuration\n";
    file << content;
    REXLOG_INFO("Saved config to {}", config_path.string());
  } catch (const std::exception& e) {
    REXLOG_ERROR("SaveConfig: {}", e.what());
  }
}

namespace testing {

ScopedLifecycleOverride::ScopedLifecycleOverride() {
  g_lifecycle_override = true;
}

ScopedLifecycleOverride::~ScopedLifecycleOverride() {
  g_lifecycle_override = false;
}

void ResetAllForTesting() {
  ResetAllToDefaults();
  ClearPendingRestartFlags();
  GetPendingValuesStorage().clear();
  g_init_done = false;
  g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
