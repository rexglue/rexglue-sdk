/**
 * @file        cvar.h
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
#include <charconv>
#include <concepts>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/cvar/config.h>
#include <rex/cvar/registry.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {
struct Config;
}

namespace rex::cvar {

/** Value type tag. */
enum class Type : u8 {
  kBool,    /**< A bool value. */
  kInt32,   /**< An i32 value. */
  kInt64,   /**< An i64 value. */
  kUInt32,  /**< A u32 value. */
  kUInt64,  /**< A u64 value. */
  kDouble,  /**< An f64 value. */
  kString,  /**< A string of at most 255 bytes. */
  kCommand, /**< A console command with no value. */
};

/** When a cvar may change. */
enum class Lifecycle : u8 {
  kHotReload,       /**< Changes take effect at once. */
  kInitOnly,        /**< Rejects every change after Init completes. */
  kRequiresRestart, /**< Changes after Init completes mark a restart pending. */
};

/** Error codes under ErrorCategory::kConfig. */
enum class CvarError : u8 {
  kNotFound = 1,   /**< No cvar or command of that name is registered. */
  kInvalid,        /**< The value does not parse or breaks a constraint. */
  kInitOnly,       /**< A change of an InitOnly cvar after Init completes. */
  kLocked,         /**< The cvar is locked. */
  kDuplicateName,  /**< A cvar of that name is already registered or appears twice. */
  kDuplicateScope, /**< A table for that scope is already registered. */
  kBadName,        /**< The scope or name is not lowercase scope.leaf form. */
  kNotACommand,    /**< Invoke on a var. */
  kNotAVar,        /**< A value operation on a command. */
};

/**
 * Builds a Config category error with a cvar error code.
 *
 * @param code The cvar error code.
 * @param message The error text.
 * @return The error.
 */
inline rex::Error BuildError(CvarError code, std::string message) {
  return rex::Error(ErrorCategory::kConfig, std::move(message), static_cast<int>(code));
}

/** Value constraints. Constant data. */
struct Constraints {
  /** Smallest numeric value, or none. */
  std::optional<f64> min;
  /** Largest numeric value, or none. */
  std::optional<f64> max;
  /** Allowed text values. Empty allows any value. */
  std::span<const std::string_view> allowed;
  /** Extra check on the text form, or null. */
  bool (*validator)(std::string_view) = nullptr;
};

/** Everything the registry and the console know about a cvar. */
struct CvarDesc {
  /** Dotted scope.leaf name such as gpu.vsync. */
  std::string_view name;
  /** Scope part of the name. */
  std::string_view scope;
  /** Value type. */
  Type type = Type::kBool;
  /** Text the console and the settings overlay show. */
  std::string_view description;
  /** When the value may change. */
  Lifecycle lifecycle = Lifecycle::kHotReload;
  /** Value constraints. */
  Constraints constraints;
  /** True shows the cvar only in debug builds of the settings overlay. */
  bool debug_only = false;
  /** True excludes the cvar from Serialize and SaveConfigFile. */
  bool no_save = false;
};

class DetailAccess;

/**
 * Type erased view of a Var<T> or Command. Registered by pointer, never copied.
 */
class VarBase {
 public:
  /** Constructs a var that no registry holds. */
  constexpr VarBase() noexcept = default;
  /** Copies nothing but the vtable. Chain methods copy a Var before any registration. */
  constexpr VarBase(const VarBase&) noexcept {}
  VarBase& operator=(const VarBase&) = delete;
  virtual ~VarBase() = default;
  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  virtual const CvarDesc& GetDesc() const = 0;
  /**
   * Returns the default as text.
   *
   * @return The default text, from storage that lasts as long as the var.
   */
  virtual std::string_view GetDefaultText() const = 0;
  /**
   * Parses text into the value without checking constraints.
   *
   * @param text The value as text.
   * @return Ok, or Invalid when the text does not parse.
   */
  [[nodiscard]] virtual Result<void> Parse(std::string_view text) = 0;
  /**
   * Returns the current value as text.
   *
   * @return The current value as text.
   */
  virtual std::string Render() const = 0;
  /**
   * Runs a command.
   *
   * @param args The argument text.
   * @return Ok, or NotACommand on a var.
   */
  [[nodiscard]] virtual Result<void> Invoke(std::string_view args) = 0;
  /**
   * Returns the registry that holds this var. Set by Register, cleared by Unregister.
   *
   * @return The registry, or null.
   */
  ICvarRegistry* GetRegistry() const { return registry_.load(std::memory_order_acquire); }

 private:
  friend class DetailAccess;
  mutable std::atomic<ICvarRegistry*> registry_ = nullptr;
  mutable std::atomic<bool> reported_ = false;
};

/**
 * Lets the registry implementation set VarBase::registry_. Nothing else uses it.
 */
class DetailAccess {
 public:
  /**
   * Stores the registry that holds a var.
   *
   * @param var The var.
   * @param registry The registry, or null.
   */
  static void SetRegistry(const VarBase& var, ICvarRegistry* registry) {
    var.registry_.store(registry, std::memory_order_release);
  }
  /**
   * Marks a var as reported once.
   *
   * @param var The var.
   * @return True for the first call on the var, false after.
   */
  static bool MarkReported(const VarBase& var) {
    return !var.reported_.exchange(true, std::memory_order_relaxed);
  }
};

namespace detail {

/**
 * Logs an error the first time a var is read while no registry holds it, once every module has
 * connected. Debug builds call it from Var::Get.
 *
 * @param var The var that was read.
 */
REX_API void ReportUnregistered(const VarBase& var);

/** Renders a scalar default at compile time into a fixed buffer. */
struct DefaultText {
  std::array<char, 32> text{};
  u8 size = 0;
  constexpr std::string_view GetView() const { return {text.data(), size}; }
};

constexpr DefaultText RenderDefault(bool v) noexcept {
  DefaultText out;
  std::string_view s = v ? "true" : "false";
  for (char c : s)
    out.text[out.size++] = c;
  return out;
}

template <typename T>
  requires std::integral<T> && (!std::same_as<T, bool>)
constexpr DefaultText RenderDefault(T v) noexcept {
  DefaultText out;
  bool negative = v < 0;
  using U = std::make_unsigned_t<T>;
  U mag = negative ? static_cast<U>(0) - static_cast<U>(v) : static_cast<U>(v);
  std::array<char, 24> tmp{};
  i32 n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + mag % 10);
    mag /= 10;
  } while (mag);
  if (negative)
    out.text[out.size++] = '-';
  while (n)
    out.text[out.size++] = tmp[--n];
  return out;
}

constexpr DefaultText RenderDefault(f64 v) noexcept {
  DefaultText out;
  bool negative = v < 0;
  if (negative)
    v = -v;
  auto whole = static_cast<u64>(v);
  f64 frac = v - static_cast<f64>(whole);
  auto whole_text = RenderDefault<u64>(whole);
  if (negative)
    out.text[out.size++] = '-';
  for (char c : whole_text.GetView())
    out.text[out.size++] = c;
  out.text[out.size++] = '.';
  for (i32 i = 0; i < 6; ++i) {
    frac *= 10.0;
    auto digit = static_cast<i32>(frac);
    out.text[out.size++] = static_cast<char>('0' + digit);
    frac -= digit;
  }
  while (out.size > 0 && out.text[out.size - 1] == '0')
    --out.size;
  if (out.size > 0 && out.text[out.size - 1] == '.')
    --out.size;
  return out;
}

/** Fixed capacity string cell so Var<std::string> stays a literal type. */
struct StringCell {
  static constexpr size_t kCapacity = 255;
  std::array<char, kCapacity + 1> text{};
  u8 size = 0;
  constexpr StringCell() noexcept = default;
  constexpr StringCell(std::string_view s) noexcept {
    for (char c : s.substr(0, kCapacity))
      text[size++] = c;
  }
  constexpr std::string_view GetView() const { return {text.data(), size}; }
};

template <typename T>
constexpr Type GetType() noexcept {
  if constexpr (std::same_as<T, bool>)
    return Type::kBool;
  else if constexpr (std::same_as<T, i32>)
    return Type::kInt32;
  else if constexpr (std::same_as<T, i64>)
    return Type::kInt64;
  else if constexpr (std::same_as<T, u32>)
    return Type::kUInt32;
  else if constexpr (std::same_as<T, u64>)
    return Type::kUInt64;
  else if constexpr (std::same_as<T, f64>)
    return Type::kDouble;
  else
    return Type::kString;
}

/**
 * Parses cvar text into a value. Bool accepts true, 1, yes, on and false, 0, no, off.
 *
 * @tparam T The value type.
 * @param text The value as text.
 * @return The parsed value, or std::nullopt when the text does not parse.
 */
template <typename T>
std::optional<T> ParseValue(std::string_view text);

template <typename... Args>
constexpr size_t CountArgs(Args&&...) noexcept {
  return sizeof...(Args);
}

}  // namespace detail

/**
 * A cvar with typed storage. Literal type, constant initialized, no constructor runs at load.
 * Scalar reads are relaxed atomic loads through std::atomic_ref. String reads copy under a
 * spinlock.
 *
 * @tparam T The value type, 1 of bool, i32, i64, u32, u64, f64 or std::string.
 *
 * @code
 * REXCVAR_BOOL(gpu, vsync, true, "Wait for vertical blank before presenting");
 * @endcode
 */
template <typename T>
class Var final : public VarBase {
 public:
  /** The stored type. */
  using value_type = T;
  /** True for Var<std::string>. */
  static constexpr bool kIsString = std::same_as<T, std::string>;
  /** The storage cell, a fixed buffer for strings and T otherwise. */
  using Cell = std::conditional_t<kIsString, detail::StringCell, T>;
  /** The default value parameter type, a view for strings and T otherwise. */
  using Init = std::conditional_t<kIsString, std::string_view, T>;

  /**
   * Constructs a var holding its default.
   *
   * @param name The dotted scope.leaf name, a string literal.
   * @param default_value The default, rendered to text at compile time.
   * @param description Shown by the console and the settings overlay.
   */
  constexpr Var(std::string_view name, Init default_value, std::string_view description) noexcept
      : default_text_(BuildDefaultText(default_value)),
        default_string_(BuildDefaultString(default_value)),
        cell_(BuildCell(default_value)) {
    desc_.name = name;
    desc_.scope = name.substr(0, name.find('.'));
    desc_.type = detail::GetType<T>();
    desc_.description = description;
  }
  constexpr Var(const Var&) noexcept = default;

  /**
   * Sets an inclusive numeric range.
   *
   * @param min_value The smallest value.
   * @param max_value The largest value.
   * @return This var with the range set.
   */
  constexpr Var SetRange(f64 min_value, f64 max_value) && noexcept {
    desc_.constraints.min = min_value;
    desc_.constraints.max = max_value;
    return *this;
  }
  /**
   * Sets the allowed values.
   *
   * @param values Values with static storage, REXCVAR_ALLOWED provides them.
   * @return This var with the allowed values set.
   */
  constexpr Var SetAllowed(std::span<const std::string_view> values) && noexcept {
    desc_.constraints.allowed = values;
    return *this;
  }
  /**
   * Sets when the value may change.
   *
   * @param lc The lifecycle.
   * @return This var with the lifecycle set.
   */
  constexpr Var SetLifecycle(Lifecycle lc) && noexcept {
    desc_.lifecycle = lc;
    return *this;
  }
  /**
   * Adds validation on the text form.
   *
   * @param fn Returns false for text the var rejects.
   * @return This var with the validator set.
   */
  constexpr Var SetValidator(bool (*fn)(std::string_view)) && noexcept {
    desc_.constraints.validator = fn;
    return *this;
  }
  /**
   * Shows the cvar only in debug builds of the settings overlay.
   *
   * @return This var with CvarDesc::debug_only set.
   */
  constexpr Var SetDebugOnly() && noexcept {
    desc_.debug_only = true;
    return *this;
  }

  /**
   * Marks the cvar as never written to a config file.
   *
   * @return This var with CvarDesc::no_save set.
   */
  constexpr Var SetNoSave() && noexcept {
    desc_.no_save = true;
    return *this;
  }

  /**
   * Returns the current value.
   *
   * @return The current value.
   */
  T Get() const;
  /**
   * Sets the cvar.
   *
   * @param value The new value.
   * @return Ok, NotFound when the var is not registered, otherwise as ICvarRegistry::Set.
   */
  [[nodiscard]] Result<void> Set(const T& value);

  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  const CvarDesc& GetDesc() const override { return desc_; }
  /**
   * Returns the default as text.
   *
   * @return The default text.
   */
  std::string_view GetDefaultText() const override {
    if constexpr (kIsString)
      return default_string_.GetView();
    else
      return default_text_.GetView();
  }
  /**
   * Parses text into the cell.
   *
   * @param text The value as text.
   * @return Ok, or Invalid when the text does not parse.
   */
  [[nodiscard]] Result<void> Parse(std::string_view text) override;
  /**
   * Returns the current value as text.
   *
   * @return The current value as text.
   */
  std::string Render() const override;
  /**
   * Rejects the call, a var is not a command.
   *
   * @return NotACommand.
   */
  [[nodiscard]] Result<void> Invoke(std::string_view) override {
    return Err(BuildError(CvarError::kNotACommand, std::string(desc_.name) + " is not a command"));
  }

 private:
  static constexpr detail::DefaultText BuildDefaultText(Init v) noexcept {
    if constexpr (kIsString)
      return {};
    else
      return detail::RenderDefault(v);
  }
  static constexpr detail::StringCell BuildDefaultString(Init v) noexcept {
    if constexpr (kIsString)
      return detail::StringCell(v);
    else
      return {};
  }
  static constexpr Cell BuildCell(Init v) noexcept {
    if constexpr (kIsString)
      return detail::StringCell(v);
    else
      return v;
  }
  static std::string Render(const T& value) {
    if constexpr (std::same_as<T, bool>)
      return value ? "true" : "false";
    else if constexpr (kIsString)
      return value;
    else if constexpr (std::same_as<T, f64>)
      return std::format("{}", value);
    else
      return std::to_string(value);
  }
  void Store(const T& value);

  CvarDesc desc_;
  detail::DefaultText default_text_;
  detail::StringCell default_string_;
  mutable Cell cell_;
  mutable u8 lock_ = 0;
};

/**
 * A console command. Constant initialized.
 *
 * @code
 * REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Print the arguments");
 * @endcode
 */
class Command final : public VarBase {
 public:
  /** The command function. */
  using Fn = void (*)(std::string_view args);
  /**
   * Constructs a command bound to a function.
   *
   * @param name The dotted scope.leaf name, a string literal.
   * @param fn Runs with the argument text.
   * @param description Shown by the console.
   */
  constexpr Command(std::string_view name, Fn fn, std::string_view description) noexcept : fn_(fn) {
    desc_.name = name;
    desc_.scope = name.substr(0, name.find('.'));
    desc_.type = Type::kCommand;
    desc_.description = description;
  }
  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  const CvarDesc& GetDesc() const override { return desc_; }
  /**
   * Returns the placeholder default text.
   *
   * @return The literal "<command>".
   */
  std::string_view GetDefaultText() const override { return "<command>"; }
  /**
   * Rejects the call, a command holds no value.
   *
   * @return NotAVar.
   */
  [[nodiscard]] Result<void> Parse(std::string_view) override {
    return Err(BuildError(CvarError::kNotAVar, std::string(desc_.name) + " is a command"));
  }
  /**
   * Returns the placeholder value text.
   *
   * @return The literal "<command>".
   */
  std::string Render() const override { return "<command>"; }
  /**
   * Runs the command.
   *
   * @param args The argument text.
   * @return Ok.
   */
  [[nodiscard]] Result<void> Invoke(std::string_view args) override {
    fn_(args);
    return Ok();
  }

 private:
  CvarDesc desc_;
  Fn fn_;
};

/**
 * Reports whether a var holds a value other than its default.
 *
 * @param var The var or command to inspect.
 * @return True when the var is registered and its value came from anywhere but the default.
 */
inline bool IsSet(const VarBase& var) {
  auto* registry = var.GetRegistry();
  return registry && registry->GetSource(var.GetDesc().name) != CvarSource::kDefault;
}

/**
 * Reads every REX_<SCOPE>_<LEAF> environment variable into config.cvars.values. The first
 * underscore separates the scope from the leaf.
 *
 * @param config The configuration to fill.
 */
REX_API void ApplyEnvironment(rex::Config& config);

/**
 * Clamps the text of an integer cvar to its type range and constraints.
 *
 * @param desc The cvar, of type Int32, Int64, UInt32 or UInt64.
 * @param text The value text. Text that does not parse reads as 0.
 * @return The clamped value text.
 */
REX_API std::string ClampIntegerCvarText(const rex::cvar::CvarDesc& desc, std::string_view text);

/**
 * Reports whether text is 1 part of a cvar name, which starts with a lowercase letter and holds
 * lowercase letters, digits and underscores.
 *
 * @param part The scope or the leaf.
 * @return True when part is a valid name part.
 */
REX_API bool IsNamePart(std::string_view part);

/**
 * Reports whether text is a cvar name, a scope and a leaf joined by 1 dot.
 *
 * @param name The text to check.
 * @return True when both parts are valid name parts.
 */
REX_API bool IsName(std::string_view name);

namespace detail {

template <typename... Vars>
constexpr std::array<const VarBase*, sizeof...(Vars)> BuildTable(const Vars&... vars) noexcept {
  return {static_cast<const VarBase*>(&vars)...};
}

template <>
inline std::optional<bool> ParseValue<bool>(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
    return true;
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
    return false;
  return std::nullopt;
}

template <typename T>
std::optional<T> ParseValue(std::string_view text) {
  if constexpr (std::same_as<T, f64>) {
    f64 v = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || ptr != text.data() + text.size())
      return std::nullopt;
    return v;
  } else if constexpr (std::same_as<T, std::string>) {
    if (text.size() > StringCell::kCapacity)
      return std::nullopt;
    return std::string(text);
  } else {
    T v{};
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || ptr != text.data() + text.size())
      return std::nullopt;
    return v;
  }
}

}  // namespace detail

template <typename T>
T Var<T>::Get() const {
#ifndef NDEBUG
  if (!GetRegistry()) {
    detail::ReportUnregistered(*this);
  }
#endif
  if constexpr (kIsString) {
    std::atomic_ref<u8> lock(lock_);
    while (lock.exchange(1, std::memory_order_acquire)) {}
    std::string out(cell_.GetView());
    lock.store(0, std::memory_order_release);
    return out;
  } else {
    return std::atomic_ref<Cell>(cell_).load(std::memory_order_relaxed);
  }
}

template <typename T>
void Var<T>::Store(const T& value) {
  if constexpr (kIsString) {
    std::atomic_ref<u8> lock(lock_);
    while (lock.exchange(1, std::memory_order_acquire)) {}
    cell_ = detail::StringCell(value);
    lock.store(0, std::memory_order_release);
  } else {
    std::atomic_ref<Cell>(cell_).store(value, std::memory_order_relaxed);
  }
}

template <typename T>
Result<void> Var<T>::Set(const T& value) {
  auto* reg = GetRegistry();
  if (!reg) {
    return Err(BuildError(CvarError::kNotFound, std::string(desc_.name) + " is not registered"));
  }
  return reg->Set(desc_.name, Render(value));
}

template <typename T>
Result<void> Var<T>::Parse(std::string_view text) {
  auto parsed = detail::ParseValue<T>(text);
  if (!parsed) {
    return Err(BuildError(CvarError::kInvalid,
                          std::string(desc_.name) + " cannot parse '" + std::string(text) + "'"));
  }
  Store(*parsed);
  return Ok();
}

template <typename T>
std::string Var<T>::Render() const {
  return Render(Get());
}

}  // namespace rex::cvar

/**
 * Declares a bool cvar as an inline ::rex::cvar::Var<bool> named name.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_BOOL(gpu, vsync, true, "Wait for vertical blank before presenting");
 * @endcode
 */
#define REXCVAR_BOOL(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<bool> name =              \
      ::rex::cvar::Var<bool>(#scope "." #name, default_value, description)
/**
 * Declares an i32 cvar.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_INT32(gpu, scale, 1, "Resolution scale");
 * @endcode
 */
#define REXCVAR_INT32(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<::rex::i32> name =         \
      ::rex::cvar::Var<::rex::i32>(#scope "." #name, default_value, description)
/**
 * Declares an i64 cvar.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_INT64(kernel, start_value, 0, "Start value");
 * @endcode
 */
#define REXCVAR_INT64(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<::rex::i64> name =         \
      ::rex::cvar::Var<::rex::i64>(#scope "." #name, default_value, description)
/**
 * Declares a u32 cvar.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_UINT32(audio, channels, 2, "Output channel count");
 * @endcode
 */
#define REXCVAR_UINT32(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<::rex::u32> name =          \
      ::rex::cvar::Var<::rex::u32>(#scope "." #name, default_value, description)
/**
 * Declares a u64 cvar.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_UINT64(kernel, heap_bytes, 0, "Heap size in bytes");
 * @endcode
 */
#define REXCVAR_UINT64(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<::rex::u64> name =          \
      ::rex::cvar::Var<::rex::u64>(#scope "." #name, default_value, description)
/**
 * Declares an f64 cvar.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_DOUBLE(ui, cursor_timeout, 0.0, "Seconds before the cursor hides");
 * @endcode
 */
#define REXCVAR_DOUBLE(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<::rex::f64> name =          \
      ::rex::cvar::Var<::rex::f64>(#scope "." #name, default_value, description)
/**
 * Declares a string cvar of at most 255 bytes.
 *
 * @param scope Cvar scope, an identifier.
 * @param name Cvar leaf and variable name, an identifier.
 * @param default_value The default.
 * @param description Shown by the console and the settings overlay.
 *
 * @code
 * REXCVAR_STRING(fs, root, "", "Content root");
 * @endcode
 */
#define REXCVAR_STRING(scope, name, default_value, description) \
  inline constinit ::rex::cvar::Var<std::string> name =         \
      ::rex::cvar::Var<std::string>(#scope "." #name, default_value, description)
/**
 * Declares a console command without arguments.
 *
 * @param scope Command scope, an identifier.
 * @param name Command leaf and variable name, an identifier.
 * @param fn Function taking no arguments.
 * @param description Shown by the console.
 *
 * @code
 * REXCVAR_COMMAND(console, clear, ConsoleClear, "Clear the console");
 * @endcode
 */
#define REXCVAR_COMMAND(scope, name, fn, description)             \
  inline constinit ::rex::cvar::Command name {                    \
    #scope "." #name, [](std::string_view) { fn(); }, description \
  }
/**
 * Declares a console command taking the argument text.
 *
 * @param scope Command scope, an identifier.
 * @param name Command leaf and variable name, an identifier.
 * @param fn Function taking std::string_view.
 * @param description Shown by the console.
 *
 * @code
 * REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Print the arguments");
 * @endcode
 */
#define REXCVAR_COMMAND_ARGS(scope, name, fn, description) \
  inline constinit ::rex::cvar::Command name {             \
    #scope "." #name, fn, description                      \
  }
/**
 * Builds static storage for an allowed value list, for use with .SetAllowed().
 *
 * @param ... The allowed values as string literals.
 *
 * @code
 * .SetAllowed(REXCVAR_ALLOWED("none", "fast", "some", "full"))
 * @endcode
 */
#define REXCVAR_ALLOWED(...)                                                                       \
  ([]() constexpr noexcept -> ::std::span<const ::std::string_view> {                              \
    static constexpr ::std::array<::std::string_view, ::rex::cvar::detail::CountArgs(__VA_ARGS__)> \
        values{__VA_ARGS__};                                                                       \
    return values;                                                                                 \
  }())
/**
 * Declares a module's cvar table.
 *
 * @param table Name of the table variable.
 * @param ... The cvars in the table.
 *
 * @code
 * REXCVAR_TABLE(kGPUCvars, vsync, resolution_scale_x);
 * @endcode
 */
#define REXCVAR_TABLE(table, ...) \
  inline constexpr auto table = ::rex::cvar::detail::BuildTable(__VA_ARGS__)
/**
 * Reads a cvar.
 *
 * @param name The cvar variable.
 *
 * @code
 * bool vsync = REXCVAR_GET(vsync);
 * @endcode
 */
#define REXCVAR_GET(name) ((name).Get())
/**
 * Sets a cvar.
 *
 * @param name The cvar variable.
 * @param value The new value.
 *
 * @code
 * REXCVAR_SET(vsync, false);
 * @endcode
 */
#define REXCVAR_SET(name, value) ((name).Set(value))
