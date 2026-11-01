/**
 * @file        logging/log.h
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
class REX_API Category : public NonCopyable {
 public:
  /** Slot value of a copy that has not written yet. */
  static constexpr u16 kUnresolved = 0xFFFF;

  /**
   * Builds the descriptor.
   *
   * @param name Identifier, lowercase, unique across the process.
   * @param description Shown by the console.
   */
  constexpr Category(std::string_view name, std::string_view description) noexcept
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

REX_API void Write(const Category& category, Level level, std::string_view text);

/** Writes the line that replaces a message that threw. Falls back to stderr. */
REX_API void WriteFailure(const Category& category, const char* what) noexcept;

/** Formats and writes a message. A throw becomes a WriteFailure line. */
REX_API void WriteArgs(const Category& category, Level level, std::string_view fmt,
                       std::format_args args) noexcept;

/** Formats and writes a message. A throw becomes a WriteFailure line. */
template <typename... Args>
void WriteFormatted(const Category& category, Level level, std::format_string<Args...> fmt,
                    Args&&... args) noexcept {
  WriteArgs(category, level, fmt.get(), std::make_format_args(args...));
}

/**
 * Writes a stub call. The first call of each stub is a warning, later calls are Dev.
 *
 * @param seen Per stub flag, set by the first call.
 * @param detail Text after the name, may be empty.
 */
REX_API void WriteStub(const Category& category, std::atomic_flag& seen, std::string_view name,
                       std::string_view detail);

/** Collects categories for Logger::AddCategories. */
template <typename... Categories>
constexpr std::array<const Category*, sizeof...(Categories)> BuildTable(
    const Categories&... categories) noexcept {
  return {static_cast<const Category*>(&categories)...};
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
#define REXLOG_CATEGORY(name, description)                        \
  namespace rex::log {                                            \
  inline constinit ::rex::log::Category name{#name, description}; \
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

/** Remembers which keys in [0, Count) have been seen, for REXLOG_ONCE. Thread safe. */
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
void NoThrow(const Category& category, Fn&& fn) noexcept {
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
 * @param level A rex::log::Level.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DETAIL_WRITE(gpu, ::rex::log::Level::kInfo, "{} pipelines", count);
 * @endcode
 */
#define REXLOG_DETAIL_WRITE(category, level, fmt, ...)                    \
  do {                                                                    \
    if (::rex::log::category.IsEnabled(level)) {                          \
      ::rex::log::detail::WriteFormatted(::rex::log::category, level,     \
                                         fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                     \
  } while (0)

/**
 * Writes a message when the category is enabled and the key has not written yet.
 *
 * @param level A rex::log::Level.
 * @param once A rex::log::OncePerKey lvalue.
 * @param key The key, written once per distinct value.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_ONCE(gpu, ::rex::log::Level::kWarn, seen_, index, "register {:04X}", index);
 * @endcode
 */
#define REXLOG_ONCE(category, level, once, key, fmt, ...)                 \
  do {                                                                    \
    if (::rex::log::category.IsEnabled(level) && (once).IsFirst(key)) {   \
      ::rex::log::detail::WriteFormatted(::rex::log::category, level,     \
                                         fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                     \
  } while (0)

/**
 * Writes a Warn message once per key.
 *
 * @param once A rex::log::OncePerKey lvalue.
 * @param key The key, written once per distinct value.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_WARN_ONCE(apu, write_once_, index, "write to unknown register {:04X}", index);
 * @endcode
 */
#define REXLOG_WARN_ONCE(category, once, key, fmt, ...) \
  REXLOG_ONCE(category, ::rex::log::Level::kWarn, once, key, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Trace message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_TRACE(gpu, "packet {:08X}", header);
 * @endcode
 */
#define REXLOG_TRACE(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kTrace, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Dev message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DEV(title, "{}", text);
 * @endcode
 */
#define REXLOG_DEV(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kDev, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Debug message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_DEBUG(system, "thread {} created", id);
 * @endcode
 */
#define REXLOG_DEBUG(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kDebug, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Info message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_INFO(ui, "SDL video driver {}", name);
 * @endcode
 */
#define REXLOG_INFO(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kInfo, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Warn message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_WARN(audio, "falling back to {} channels", channels);
 * @endcode
 */
#define REXLOG_WARN(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kWarn, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_ERROR(filesystem, "{} could not be opened", path);
 * @endcode
 */
#define REXLOG_ERROR(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kError, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes a Critical message.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REXLOG_CRITICAL(core, "heap corrupted at {:08X}", address);
 * @endcode
 */
#define REXLOG_CRITICAL(category, fmt, ...) \
  REXLOG_DETAIL_WRITE(category, ::rex::log::Level::kCritical, fmt __VA_OPT__(, ) __VA_ARGS__)

/**
 * Writes an Error message and returns a value from the enclosing function.
 *
 * @param value The expression the enclosing function returns.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FAIL(kernel, X_STATUS_INVALID_PARAMETER, "{} is out of range", index);
 * @endcode
 */
#define REX_FAIL(category, value, fmt, ...)                 \
  do {                                                      \
    REXLOG_ERROR(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    return value;                                           \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FATAL(system, "heap {} is corrupted", name);
 * @endcode
 */
#define REX_FATAL(category, fmt, ...)                          \
  do {                                                         \
    REXLOG_CRITICAL(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    ::rex::log::Flush();                                       \
    ::std::abort();                                            \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts when a condition is false.
 *
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_CHECK(system, base != nullptr, "the guest memory base is not mapped");
 * @endcode
 */
#define REX_CHECK(category, cond, fmt, ...)                                            \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      REX_FATAL(category, "check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                                  \
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
