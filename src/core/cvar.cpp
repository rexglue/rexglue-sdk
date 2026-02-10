/**
 * @file        cvar.cpp
 * @brief       Configuration variable system implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <rex/cvar.h>
#include <rex/logging.h>

#include <CLI/CLI.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace rex::cvar {

namespace {

bool g_finalized = false;
bool g_lifecycle_override = false;
std::mutex g_mutex;

// Flag registry - use functions to avoid static init order issues
std::vector<FlagEntry>& GetRegistryStorage() {
    static std::vector<FlagEntry> registry;
    return registry;
}

std::unordered_map<std::string, size_t>& GetRegistryIndex() {
    static std::unordered_map<std::string, size_t> index;
    return index;
}

// Convert flag name to environment variable: gpu_vsync -> REX_GPU_VSYNC
std::string FlagNameToEnvVar(std::string_view name) {
    std::string result = "REX_";
    for (char c : name) {
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
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

            if (SetFlagByName(full_key, value_str)) {
                REXLOG_DEBUG("Config: {} = {}", full_key, value_str);
            } else {
                REXLOG_WARN("Config: unknown cvar '{}'", full_key);
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
            if (!ParseDouble(value, numeric_val)) return false;
        } else {
            // Integer types
            int64_t int_val = 0;
            auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), int_val);
            if (ec != std::errc()) return false;
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

void RegisterFlag(FlagEntry entry) {
    auto it = GetRegistryIndex().find(entry.name);
    if (it != GetRegistryIndex().end()) {
        return;  // Already registered
    }

    GetRegistryIndex()[entry.name] = GetRegistryStorage().size();
    GetRegistryStorage().push_back(std::move(entry));
}

bool SetFlagByName(std::string_view name, std::string_view value) {
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
        return false;
    }

    const auto& entry = GetRegistryStorage()[it->second];

    // Check lifecycle
    if (!g_lifecycle_override && entry.lifecycle == Lifecycle::kInitOnly && IsFinalized()) {
        REXLOG_WARN("Cannot modify init-only flag '{}' after initialization", name);
        return false;
    }

    // Validate constraints
    if (!ValidateConstraints(entry, value)) {
        return false;
    }

    bool success = entry.setter(value);

    // Track pending restart flags
    if (success && entry.lifecycle == Lifecycle::kRequiresRestart) {
        MarkPendingRestart(name);
    }

    // Invoke registered callbacks
    if (success) {
        auto& callbacks = GetCallbackStorage();
        auto it = callbacks.find(std::string(name));
        if (it != callbacks.end()) {
            for (const auto& callback : it->second) {
                callback(name, value);
            }
        }
    }

    return success;
}

std::string GetFlagByName(std::string_view name) {
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
        return "";
    }

    return GetRegistryStorage()[it->second].getter();
}

std::vector<std::string> ListFlags() {
    std::vector<std::string> result;
    result.reserve(GetRegistryStorage().size());
    for (const auto& entry : GetRegistryStorage()) {
        result.push_back(entry.name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> ListFlagsByCategory(std::string_view category) {
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
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
        return nullptr;
    }
    return &GetRegistryStorage()[it->second];
}

std::vector<std::string> GetPendingRestartFlags() {
    return GetPendingRestartStorage();
}

void ClearPendingRestartFlags() {
    GetPendingRestartStorage().clear();
}

void ResetToDefault(std::string_view name) {
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
        return;
    }
    const auto& entry = GetRegistryStorage()[it->second];
    entry.setter(entry.default_value);
}

void ResetAllToDefaults() {
    for (const auto& entry : GetRegistryStorage()) {
        entry.setter(entry.default_value);
    }
}

bool HasNonDefaultValue(std::string_view name) {
    auto it = GetRegistryIndex().find(std::string(name));
    if (it == GetRegistryIndex().end()) {
        return false;
    }
    const auto& entry = GetRegistryStorage()[it->second];
    return entry.getter() != entry.default_value;
}

std::vector<std::string> ListModifiedFlags() {
    std::vector<std::string> result;
    for (const auto& entry : GetRegistryStorage()) {
        if (entry.getter() != entry.default_value) {
            result.push_back(entry.name);
        }
    }
    return result;
}

std::string SerializeToTOML() {
    std::string result;
    for (const auto& entry : GetRegistryStorage()) {
        if (entry.getter() != entry.default_value) {
            if (entry.type == FlagType::String) {
                result += entry.name + " = \"" + entry.getter() + "\"\n";
            } else {
                result += entry.name + " = " + entry.getter() + "\n";
            }
        }
    }
    return result;
}

std::string SerializeToTOML(std::string_view category) {
    std::string result;
    for (const auto& entry : GetRegistryStorage()) {
        if (entry.category == category && entry.getter() != entry.default_value) {
            if (entry.type == FlagType::String) {
                result += entry.name + " = \"" + entry.getter() + "\"\n";
            } else {
                result += entry.name + " = " + entry.getter() + "\n";
            }
        }
    }
    return result;
}

void RegisterChangeCallback(std::string_view name, ChangeCallback callback) {
    GetCallbackStorage()[std::string(name)].push_back(std::move(callback));
}

void UnregisterChangeCallbacks(std::string_view name) {
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
                    entry.setter(count > 0 ? "true" : "false");
                },
                entry.description);
        } else {
            app.add_option_function<std::string>(
                "--" + entry.name,
                [&entry](const std::string& val) {
                    entry.setter(val);
                },
                entry.description);
        }
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        REXLOG_WARN("cvar: CLI parse issue: {}", e.what());
    }

    REXLOG_DEBUG("cvar: parsed command line, {} cvars registered",
                 GetRegistryStorage().size());

    return app.remaining();
}

void LoadConfig(const std::filesystem::path& config_path) {
    if (!std::filesystem::exists(config_path)) {
        REXLOG_DEBUG("Config file not found: {}", config_path.string());
        return;
    }

    try {
        auto config = toml::parse_file(config_path.string());
        ApplyTomlTable(config, "");
        REXLOG_INFO("Loaded config from {}", config_path.string());
    } catch (const toml::parse_error& err) {
        REXLOG_ERROR("Failed to parse config {}: {}", config_path.string(), err.what());
    }
}

void ApplyEnvironment() {
    int count = 0;
    for (const auto& entry : GetRegistryStorage()) {
        std::string env_name = FlagNameToEnvVar(entry.name);
        const char* env_value = std::getenv(env_name.c_str());
        if (env_value != nullptr) {
            if (entry.setter(env_value)) {
                REXLOG_DEBUG("Env: {} = {} (from {})", entry.name, env_value, env_name);
                ++count;
            } else {
                REXLOG_WARN("Env: failed to parse {} = {}", env_name, env_value);
            }
        }
    }

    if (count > 0) {
        REXLOG_INFO("Applied {} environment variable override(s)", count);
    }
}

void FinalizeInit() {
    std::lock_guard lock(g_mutex);
    g_finalized = true;
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
    g_finalized = false;
}

}  // namespace testing

}  // namespace rex::cvar
