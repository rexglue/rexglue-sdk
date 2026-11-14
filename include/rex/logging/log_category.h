/**
 * @file        logging/log_category.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::log {

/** Severity of a log message, least severe first. */
enum class Level : u8 {
  kTrace,    /**< Per instruction, per packet and per iteration detail. */
  kDev,      /**< Detail for people porting a title. ReX debug output and repeat stub calls. */
  kDebug,    /**< State changes and intermediate values. */
  kInfo,     /**< Operational events a user reads. At most a few lines per subsystem start. */
  kWarn,     /**< Recoverable problems, fallbacks and unsupported features. */
  kError,    /**< Failures that lose functionality. */
  kCritical, /**< Unrecoverable state, written right before an abort. */
  kOff,      /**< Threshold only. Nothing is written at it. */
};

/** Lowercase level names in level order, kOff last. */
inline constexpr std::array<std::string_view, 8> kLevelNames = {
    "trace", "dev", "debug", "info", "warn", "error", "critical", "off"};

/** Number of levels a message can carry, kTrace through kCritical. */
inline constexpr size_t kMessageLevelCount = 7;

/**
 * Parses a level name.
 *
 * @param text One of trace, dev, debug, info, warn, error, critical or off, in any case.
 * @return The level, or an error naming the text.
 */
[[nodiscard]] REX_API Result<Level> ParseLevel(std::string_view text);

/** Returns the lowercase name of a level. ParseLevel accepts the name. */
REX_API std::string_view LevelName(Level level);

}  // namespace rex::log

namespace rex::log {

class DetailAccess;

/**
 * Log category declared with REXLOG_CATEGORY and added with Logger::AddCategories. Copies with the
 * same name share one level.
 */
class REX_API LogCategory : public NonCopyable {
 public:
  /** Slot value of a copy that has not written yet. */
  static constexpr u16 kUnresolved = 0xFFFF;

  /**
   * Builds the descriptor.
   *
   * @param name Identifier, lowercase, unique across the process.
   * @param description Shown by the console.
   */
  constexpr LogCategory(std::string_view name, std::string_view description) noexcept
      : name_(name), description_(description) {}

  constexpr std::string_view GetName() const { return name_; }

  constexpr std::string_view GetDescription() const { return description_; }

  /**
   * Reports whether a message at a level would be written.
   *
   * @return True when the level is at or above the category's effective level.
   */
  bool IsEnabled(Level level) const;

 private:
  friend class DetailAccess;

  std::string_view name_;
  std::string_view description_;
  mutable std::atomic<u16> slot_ = kUnresolved;
};

struct CategoryState {
  std::string name;
  std::string description;
  Level level = Level::kInfo; /**< Effective level. */
  std::optional<Level>
      explicit_level;      /**< Level set for this category, empty when it follows the default. */
  bool registered = false; /**< False for a category written to before any module registered it. */
};

namespace detail {

REX_API void Write(const LogCategory& category, Level level, std::string_view text);

/** Writes the line that replaces a message that threw. Falls back to stderr. */
REX_API void WriteFailure(const LogCategory& category, const char* what) noexcept;

/** Formats and writes a message. A throw becomes a WriteFailure line. */
REX_API void WriteArgs(const LogCategory& category, Level level, std::string_view fmt,
                       std::format_args args) noexcept;

/** Formats and writes a message. A throw becomes a WriteFailure line. */
template <typename... Args>
void WriteFormatted(const LogCategory& category, Level level, std::format_string<Args...> fmt,
                    Args&&... args) noexcept {
  WriteArgs(category, level, fmt.get(), std::make_format_args(args...));
}

/**
 * Writes a stub call. The first call of each stub is a warning, later calls are Dev.
 *
 * @param seen Per stub flag, set by the first call.
 * @param detail Text after the name, may be empty.
 */
REX_API void WriteStub(const LogCategory& category, std::atomic_flag& seen, std::string_view name,
                       std::string_view detail);

/** Collects categories for Logger::AddCategories. */
template <typename... Categories>
constexpr std::array<const LogCategory*, sizeof...(Categories)> BuildTable(
    const Categories&... categories) noexcept {
  return {static_cast<const LogCategory*>(&categories)...};
}

}  // namespace detail

}  // namespace rex::log

/**
 * Declares the log category rex::log::name. Used at global scope in a header that every writer of
 * the category includes.
 *
 * @param name Category name, a lowercase identifier.
 * @param description Shown by the console.
 *
 * @code
 * REXLOG_CATEGORY(gpu, "GPU emulation")
 * @endcode
 */
#define REXLOG_CATEGORY(name, description)                           \
  namespace rex::log {                                               \
  inline constinit ::rex::log::LogCategory name{#name, description}; \
  }

/**
 * Declares rex::log::table, the category addresses a module passes to Logger::AddCategories.
 * Used at global scope.
 *
 * @param table The identifier of the generated table, such as kGPULog.
 * @param ... Category names.
 *
 * @code
 * REXLOG_TABLE(kGPULog, gpu)
 * rex::GetLogger().AddCategories(rex::log::kGPULog);
 * @endcode
 */
#define REXLOG_TABLE(table, ...)                                             \
  namespace rex::log {                                                       \
  inline constexpr auto table = ::rex::log::detail::BuildTable(__VA_ARGS__); \
  }

namespace rex::log {

/** Remembers which keys in [0, Count) have been seen, for REXLOG_CAT_ONCE. Thread safe. */
template <size_t Count>
class OncePerKey {
 public:
  /**
   * Marks a key as seen.
   *
   * @return True for the first call with the key, and always true for a key at or above Count.
   */
  bool IsFirst(size_t key) noexcept {
    if (key >= Count) {
      return true;
    }
    const u64 bit = u64{1} << (key % 64);
    return (words_[key / 64].fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
  }

 private:
  std::array<std::atomic<u64>, (Count + 63) / 64> words_{};
};

/** Writes buffered output of every sink. For crash handlers that hold no API handle. */
REX_API void Flush();

/**
 * Runs a teardown step and logs anything it throws instead of letting it escape.
 *
 * @tparam Fn Callable that takes no arguments.
 * @param category Category of the log line for a throw.
 */
template <typename Fn>
void NoThrow(const LogCategory& category, Fn&& fn) noexcept {
  try {
    std::forward<Fn>(fn)();
  } catch (const std::exception& error) {
    detail::WriteFormatted(category, Level::kError, "teardown step failed, {}", error.what());
  } catch (...) {
    detail::WriteFormatted(category, Level::kError, "teardown step failed, unknown exception");
  }
}

}  // namespace rex::log

/**
 * Writes a message when the category is enabled at the level, else evaluates no arguments.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param level A rex::log::Level.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DETAIL_WRITE(::rex::log::gpu, ::rex::log::Level::kInfo, "{} pipelines", count);
 * @endcode
 */
#define REXLOG_DETAIL_WRITE(cat, level, fmt, ...)                                       \
  do {                                                                                  \
    if ((cat).IsEnabled(level)) {                                                       \
      ::rex::log::detail::WriteFormatted((cat), level, fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                                   \
  } while (0)

/**
 * Writes a message when the category is enabled and the key has not written yet.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param level A rex::log::Level.
 * @param once A rex::log::OncePerKey lvalue.
 * @param key The key, written once per distinct value.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_ONCE(category, ::rex::log::Level::kWarn, seen_, index, "register {:04X}", index);
 * @endcode
 */
#define REXLOG_CAT_ONCE(cat, level, once, key, fmt, ...)                                \
  do {                                                                                  \
    if ((cat).IsEnabled(level) && (once).IsFirst(key)) {                                \
      ::rex::log::detail::WriteFormatted((cat), level, fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                                   \
  } while (0)

/**
 * Writes a Trace message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_TRACE(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_TRACE(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kTrace, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Dev message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_DEV(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_DEV(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kDev, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Debug message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_DEBUG(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_DEBUG(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kDebug, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Info message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_INFO(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_INFO(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kInfo, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Warn message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_WARN(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_WARN(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kWarn, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_ERROR(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_ERROR(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kError, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Critical message.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CAT_CRITICAL(category, "{}", text);
 * @endcode
 */
#define REXLOG_CAT_CRITICAL(cat, fmt, ...) \
  REXLOG_DETAIL_WRITE(cat, ::rex::log::Level::kCritical, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message and returns a value from the enclosing function.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param value The expression the enclosing function returns.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_CAT_FAIL(category, X_STATUS_INVALID_PARAMETER, "{} is out of range", index);
 * @endcode
 */
#define REX_CAT_FAIL(cat, value, fmt, ...)                 \
  do {                                                     \
    REXLOG_CAT_ERROR(cat, fmt __VA_OPT__(, ) __VA_ARGS__); \
    return value;                                          \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_CAT_FATAL(category, "heap {} is corrupted", name);
 * @endcode
 */
#define REX_CAT_FATAL(cat, fmt, ...)                          \
  do {                                                        \
    REXLOG_CAT_CRITICAL(cat, fmt __VA_OPT__(, ) __VA_ARGS__); \
    ::rex::log::Flush();                                      \
    ::std::abort();                                           \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts when a condition is false.
 *
 * @param cat A rex::log::LogCategory lvalue.
 * @param cond The condition that must hold.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_CAT_CHECK(category, base != nullptr, "the guest memory base is not mapped");
 * @endcode
 */
#define REX_CAT_CHECK(cat, cond, fmt, ...)                                            \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      REX_CAT_FATAL(cat, "check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                                 \
  } while (0)

REXLOG_CATEGORY(core, "Core library, provider loading, settings and the app lifecycle")
REXLOG_CATEGORY(system,
                "Kernel objects, guest threads, guest memory and modules, and the host threads and "
                "memory under them")
REXLOG_CATEGORY(apu, "APU emulation, XMA decoding and the audio output modules")
REXLOG_CATEGORY(filesystem, "Mount table, links and file providers")
REXLOG_CATEGORY(gpu, "Xenos emulation, shader translation and the host graphics paths")
REXLOG_CATEGORY(hid, "Pads, keyboard and pointer, and the hid modules")
REXLOG_CATEGORY(cpu, "Function dispatch and the PPC context")
REXLOG_CATEGORY(ui, "Windows, presenters, overlays and ImGui")
REXLOG_CATEGORY(console, "Console command output")

/**
 * Per category macros. Each prefix writes to its category and takes the arguments of the matching
 * REXLOG_CAT_ macro without the category.
 *
 * @code
 * REXGPU_INFO("{} pipelines", count);
 * REXSYS_FAIL(X_STATUS_INVALID_PARAMETER, "{} is out of range", index);
 * REXAPU_WARN_ONCE(write_once_, index, "write to unknown register {:04X}", index);
 * @endcode
 *
 * Every prefix has TRACE, DEV, DEBUG, INFO, WARN, ERROR, CRITICAL, WARN_ONCE, FAIL, FATAL and
 * CHECK. REXLOG_ is core, with REX_FAIL, REX_FATAL and REX_CHECK. REXSYS_ is system, REXFS_
 * filesystem and REXCON_ console. Kernel, title, XAM, codegen and rexglue define theirs beside
 * their categories.
 */
#define REXLOG_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXLOG_WARN_ONCE(once, key, fmt, ...)                            \
  REXLOG_CAT_ONCE(::rex::log::core, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REX_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::core, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REX_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::core, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REX_CHECK(cond, fmt, ...)                                            \
  do {                                                                       \
    if (!(cond)) {                                                           \
      REX_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                        \
  } while (0)

#define REXSYS_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_WARN_ONCE(once, key, fmt, ...)                              \
  REXLOG_CAT_ONCE(::rex::log::system, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::system, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::system, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXSYS_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXSYS_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)

#define REXAPU_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::apu, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::apu, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::apu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXAPU_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXAPU_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)

#define REXFS_TRACE(fmt, ...) \
  REXLOG_CAT_TRACE(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_DEBUG(fmt, ...) \
  REXLOG_CAT_DEBUG(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_ERROR(fmt, ...) \
  REXLOG_CAT_ERROR(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_WARN_ONCE(once, key, fmt, ...)                                   \
  REXLOG_CAT_ONCE(::rex::log::filesystem, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::filesystem, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::filesystem, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXFS_CHECK(cond, fmt, ...)                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      REXFS_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                          \
  } while (0)

#define REXGPU_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::gpu, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::gpu, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::gpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXGPU_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXGPU_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)

#define REXHID_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::hid, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::hid, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::hid, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXHID_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXHID_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)

#define REXCPU_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::cpu, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::cpu, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::cpu, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCPU_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXCPU_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)

#define REXUI_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_CRITICAL(fmt, ...) REXLOG_CAT_CRITICAL(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::ui, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::ui, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::ui, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXUI_CHECK(cond, fmt, ...)                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      REXUI_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                          \
  } while (0)

#define REXCON_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_WARN_ONCE(once, key, fmt, ...)                               \
  REXLOG_CAT_ONCE(::rex::log::console, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::console, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::console, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXCON_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXCON_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)
