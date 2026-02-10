/**
 * @file        cvar.h
 * @brief       cvar (configuration variable) system
 *
 * @section cvar_defining Defining CVars
 *
 * Define cvars in a single .cpp file using REXCVAR_DEFINE_* macros:
 * @code
 * // In flags.cpp or your module's .cpp file
 * REXCVAR_DEFINE_BOOL(my_flag, false, "Category", "Description");
 * REXCVAR_DEFINE_INT32(my_int, 42, "Category", "An integer setting");
 * REXCVAR_DEFINE_STRING(my_string, "default", "Category", "A string setting");
 * @endcode
 *
 * Available types: BOOL, INT32, INT64, UINT32, UINT64, DOUBLE, STRING
 *
 * @section cvar_declaring Declaring CVars (for use in other files)
 *
 * @code
 * // In a header or other .cpp that needs access
 * REXCVAR_DECLARE(bool, my_flag);
 * REXCVAR_DECLARE(int32_t, my_int);
 * REXCVAR_DECLARE(std::string, my_string);
 * @endcode
 *
 * @section cvar_access Getting and Setting Values
 *
 * @code
 * // Type-safe access (preferred for known cvars)
 * bool value = REXCVAR_GET(my_flag);
 * REXCVAR_SET(my_flag, true);
 *
 * // String-based access (for dynamic/runtime lookup)
 * std::string str_val = rex::cvar::GetFlagByName("my_flag");
 * rex::cvar::SetFlagByName("my_flag", "true");
 * @endcode
 *
 * @section cvar_metadata Adding Metadata
 *
 * Chain metadata methods after the DEFINE macro:
 * @code
 * REXCVAR_DEFINE_INT32(scale, 1, "GPU", "Resolution scale")
 *     .range(1, 8)
 *     .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
 *
 * REXCVAR_DEFINE_STRING(backend, "d3d12", "GPU", "Render backend")
 *     .allowed({"d3d12", "vulkan"})
 *     .lifecycle(rex::cvar::Lifecycle::kInitOnly);
 *
 * REXCVAR_DEFINE_BOOL(debug_overlay, false, "Debug", "Show overlay")
 *     .debug_only();
 * @endcode
 *
 * @section cvar_guidelines Metadata Guidelines
 *
 * - .lifecycle(kInitOnly) - Device/backend selection that cannot change after init
 * - .lifecycle(kRequiresRestart) - Settings that need restart to take effect
 * - .lifecycle(kHotReload) - Default; can change at runtime with immediate effect
 * - .range(min, max) - Numeric bounds validation
 * - .allowed({...}) - String enum validation
 * - .debug_only() - Mark as debug-only (for filtering in release UIs)
 * - .validator(fn) - Custom validation function
 *
 * @section cvar_query Querying CVars
 *
 * @code
 * // List all cvars
 * auto all = rex::cvar::ListFlags();
 *
 * // Query by category or lifecycle
 * auto gpu_flags = rex::cvar::ListFlagsByCategory("GPU");
 * auto init_only = rex::cvar::ListFlagsByLifecycle(rex::cvar::Lifecycle::kInitOnly);
 *
 * // Get metadata for a specific cvar
 * const auto* info = rex::cvar::GetFlagInfo("my_flag");
 * if (info) {
 *     // Access info->lifecycle, info->constraints, info->description, etc.
 * }
 *
 * // Check for modified values
 * auto modified = rex::cvar::ListModifiedFlags();
 * @endcode
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::cvar {

//=============================================================================
// Initialization API
//=============================================================================

std::vector<std::string> Init(int argc, char** argv);
void LoadConfig(const std::filesystem::path& config_path);
void ApplyEnvironment();
void FinalizeInit();
bool IsFinalized();
void SaveConfig(const std::filesystem::path& config_path);

//=============================================================================
// Flag Registry
//=============================================================================

enum class FlagType { Boolean, Int32, Int64, Uint32, Uint64, Double, String };

// Lifecycle: when can this flag be modified?
enum class Lifecycle {
    kInitOnly,       // Can only be set during initialization (before FinalizeInit)
    kHotReload,      // Can be changed at runtime with immediate effect
    kRequiresRestart // Can be changed, but only takes effect after restart
};

// Validation constraints
struct Constraints {
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> allowed_values;
    std::function<bool(std::string_view)> custom_validator;

    bool HasRangeConstraint() const { return min.has_value() || max.has_value(); }
    bool HasAllowedValues() const { return !allowed_values.empty(); }
};

struct FlagEntry {
    std::string name;
    FlagType type;
    std::string category;
    std::string description;
    std::function<bool(std::string_view)> setter;
    std::function<std::string()> getter;
    Lifecycle lifecycle = Lifecycle::kHotReload;
    Constraints constraints;
    std::string default_value;
    bool is_debug_only = false;
};

std::vector<FlagEntry>& GetRegistry();
void RegisterFlag(FlagEntry entry);
bool SetFlagByName(std::string_view name, std::string_view value);
std::string GetFlagByName(std::string_view name);
std::vector<std::string> ListFlags();
std::vector<std::string> ListFlagsByCategory(std::string_view category);
std::vector<std::string> ListFlagsByLifecycle(Lifecycle lc);
const FlagEntry* GetFlagInfo(std::string_view name);
std::vector<std::string> GetPendingRestartFlags();
void ClearPendingRestartFlags();
void ResetToDefault(std::string_view name);
void ResetAllToDefaults();
bool HasNonDefaultValue(std::string_view name);
std::vector<std::string> ListModifiedFlags();
std::string SerializeToTOML();
std::string SerializeToTOML(std::string_view category);

/// Callback invoked when a CVAR value changes
/// @param name The CVAR name
/// @param new_value The new value as a string
using ChangeCallback = std::function<void(std::string_view name, std::string_view new_value)>;

/// Register a callback to be invoked when a specific CVAR changes
void RegisterChangeCallback(std::string_view name, ChangeCallback callback);

/// Unregister all callbacks for a specific CVAR
void UnregisterChangeCallbacks(std::string_view name);

struct FlagRegistrar {
    FlagEntry* entry_ptr = nullptr;

    explicit FlagRegistrar(FlagEntry e) {
        // Register immediately and store pointer to the registered entry for chaining
        RegisterFlag(std::move(e));
        auto& registry = GetRegistry();
        entry_ptr = &registry.back();
    }

    // Move constructor for copy-initialization in macros
    FlagRegistrar(FlagRegistrar&& other) noexcept : entry_ptr(other.entry_ptr) {
        other.entry_ptr = nullptr;
    }

    // Chain methods are rvalue-ref-qualified to work with temporaries
    FlagRegistrar&& range(double min_val, double max_val) && {
        entry_ptr->constraints.min = min_val;
        entry_ptr->constraints.max = max_val;
        return std::move(*this);
    }

    FlagRegistrar&& allowed(std::initializer_list<std::string> values) && {
        entry_ptr->constraints.allowed_values = values;
        return std::move(*this);
    }

    FlagRegistrar&& lifecycle(Lifecycle lc) && {
        entry_ptr->lifecycle = lc;
        return std::move(*this);
    }

    FlagRegistrar&& debug_only() && {
        entry_ptr->is_debug_only = true;
        return std::move(*this);
    }

    FlagRegistrar&& validator(std::function<bool(std::string_view)> fn) && {
        entry_ptr->constraints.custom_validator = std::move(fn);
        return std::move(*this);
    }

    ~FlagRegistrar() = default;

    // Non-copyable (prevent double registration)
    FlagRegistrar(const FlagRegistrar&) = delete;
    FlagRegistrar& operator=(const FlagRegistrar&) = delete;
    FlagRegistrar& operator=(FlagRegistrar&&) = delete;
};

inline bool ParseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::string str(s);
    out = std::strtod(str.c_str(), &end);
    return end != str.c_str() && *end == '\0';
}

}  // namespace rex::cvar

//=============================================================================
// CVar Macros
//=============================================================================

// Declare a cvar (use in files that need access to a cvar defined elsewhere)
#define REXCVAR_DECLARE(type, name) extern type FLAGS_##name

// Get a cvar value
#define REXCVAR_GET(name) (FLAGS_##name)

// Set a cvar value
#define REXCVAR_SET(name, value) (FLAGS_##name = (value))

// Define cvars (use in one .cpp file per cvar)
// The FlagRegistrar registers the flag in its destructor, allowing method chaining.
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc) \
    bool FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Boolean, category, desc, \
        [](std::string_view v) { \
            bool val = (v == "true" || v == "1" || v == "yes"); \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return FLAGS_##name ? "true" : "false"; }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        (default_val) ? "true" : "false", \
        false \
    })

#define REXCVAR_DEFINE_INT32(name, default_val, category, desc) \
    int32_t FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Int32, category, desc, \
        [](std::string_view v) { \
            int32_t val = 0; \
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val); \
            if (ec != std::errc()) return false; \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return std::to_string(FLAGS_##name); }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        std::to_string(default_val), \
        false \
    })

#define REXCVAR_DEFINE_INT64(name, default_val, category, desc) \
    int64_t FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Int64, category, desc, \
        [](std::string_view v) { \
            int64_t val = 0; \
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val); \
            if (ec != std::errc()) return false; \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return std::to_string(FLAGS_##name); }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        std::to_string(default_val), \
        false \
    })

#define REXCVAR_DEFINE_UINT32(name, default_val, category, desc) \
    uint32_t FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Uint32, category, desc, \
        [](std::string_view v) { \
            uint32_t val = 0; \
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val); \
            if (ec != std::errc()) return false; \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return std::to_string(FLAGS_##name); }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        std::to_string(default_val), \
        false \
    })

#define REXCVAR_DEFINE_UINT64(name, default_val, category, desc) \
    uint64_t FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Uint64, category, desc, \
        [](std::string_view v) { \
            uint64_t val = 0; \
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val); \
            if (ec != std::errc()) return false; \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return std::to_string(FLAGS_##name); }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        std::to_string(default_val), \
        false \
    })

#define REXCVAR_DEFINE_DOUBLE(name, default_val, category, desc) \
    double FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::Double, category, desc, \
        [](std::string_view v) { \
            double val = 0; \
            if (!::rex::cvar::ParseDouble(v, val)) return false; \
            FLAGS_##name = val; \
            return true; \
        }, \
        []() { return std::to_string(FLAGS_##name); }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        std::to_string(default_val), \
        false \
    })

#define REXCVAR_DEFINE_STRING(name, default_val, category, desc) \
    std::string FLAGS_##name = (default_val); \
    static auto _cvar_reg_##name = ::rex::cvar::FlagRegistrar({ \
        #name, ::rex::cvar::FlagType::String, category, desc, \
        [](std::string_view v) { \
            FLAGS_##name = std::string(v); \
            return true; \
        }, \
        []() { return FLAGS_##name; }, \
        ::rex::cvar::Lifecycle::kHotReload, \
        {}, \
        default_val, \
        false \
    })

namespace rex::cvar {
namespace testing {

class ScopedLifecycleOverride {
public:
    ScopedLifecycleOverride();
    ~ScopedLifecycleOverride();

    ScopedLifecycleOverride(const ScopedLifecycleOverride&) = delete;
    ScopedLifecycleOverride& operator=(const ScopedLifecycleOverride&) = delete;
};

void ResetAllForTesting();

}  // namespace testing
}  // namespace rex::cvar
