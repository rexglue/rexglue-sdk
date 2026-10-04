/**
 * @file        cvar.h
 * @brief       Cvar declaration types, the REXCVAR_ macros and the values that reach Init
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

#include <rex/macros.h>
#include <rex/cvar/registry.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {
struct Config;
}

namespace rex::cvar {

/** Value type tag. */
enum class Type : u8 {
  Bool,    /**< A bool value. */
  Int32,   /**< An i32 value. */
  Int64,   /**< An i64 value. */
  UInt32,  /**< A u32 value. */
  UInt64,  /**< A u64 value. */
  Double,  /**< An f64 value. */
  String,  /**< A string of at most 255 bytes. */
  Command, /**< A console command with no value. */
};

/** When a cvar may change. */
enum class Lifecycle : u8 {
  HotReload,       /**< Changes take effect at once. */
  InitOnly,        /**< Rejects Runtime sets after Init completes. */
  RequiresRestart, /**< Changes after Init completes mark a restart pending. */
};

/** Where a cvar value came from, ascending precedence. */
enum class CvarSource : u8 {
  Default,     /**< The declared default. */
  Config,      /**< A TOML config file. */
  Environment, /**< A REX_<SCOPE>_<LEAF> environment variable. */
  CommandLine, /**< A command line option. */
  Runtime,     /**< A set after Init through the registry. */
};

/** Error codes under ErrorCategory::Config. */
enum class Error : u8 {
  NotFound = 1,   /**< No cvar or command of that name is registered. */
  Invalid,        /**< The value does not parse or breaks a constraint. */
  InitOnly,       /**< A Runtime set of an InitOnly cvar after Init completes. */
  Locked,         /**< The cvar is locked. */
  DuplicateName,  /**< A cvar of that name is already registered or appears twice. */
  DuplicateScope, /**< A table for that scope is already registered. */
  BadName,        /**< The scope or name is not lowercase scope.leaf form. */
  NotACommand,    /**< Invoke on a var. */
  NotAVar,        /**< A value operation on a command. */
};

/**
 * Builds a Config category error with a cvar error code.
 *
 * @param code The cvar error code.
 * @param message The error text.
 * @return The error.
 */
inline rex::Error MakeError(Error code, std::string message) {
  return rex::Error(ErrorCategory::Config, std::move(message), static_cast<int>(code));
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

/** Everything the registry and the console know about 1 cvar. */
struct Desc {
  /** Dotted scope.leaf name such as gpu.vsync. */
  std::string_view name;
  /** Scope part of the name. */
  std::string_view scope;
  /** Value type. */
  Type type = Type::Bool;
  /** Text the console and the settings overlay show. */
  std::string_view description;
  /** When the value may change. */
  Lifecycle lifecycle = Lifecycle::HotReload;
  /** Value constraints. */
  Constraints constraints;
  /** True shows the cvar only in debug builds of the settings overlay. */
  bool debug_only = false;
  /** True excludes the cvar from Serialize and SaveConfigFile. */
  bool no_save = false;
};

class detail_access;

/**
 * Type erased view of a Var<T> or Command. Registered by pointer, never copied.
 */
class IVar {
 public:
  /** Constructs a var that no registry holds. */
  constexpr IVar() = default;
  /** Copies nothing but the vtable. Chain methods copy a Var before any registration. */
  constexpr IVar(const IVar&) {}
  IVar& operator=(const IVar&) = delete;
  virtual ~IVar() = default;
  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  virtual const Desc& desc() const = 0;
  /**
   * Returns the default as text.
   *
   * @return The default text, from storage that lives as long as the var.
   */
  virtual std::string_view DefaultText() const = 0;
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
  ICvarRegistry* registry() const { return registry_.load(std::memory_order_acquire); }

 private:
  friend class detail_access;
  mutable std::atomic<ICvarRegistry*> registry_ = nullptr;
};

/**
 * Lets the registry implementation set IVar::registry_. Nothing else uses it.
 */
class detail_access {
 public:
  /**
   * Records the registry that holds a var.
   *
   * @param var The var.
   * @param registry The registry, or null.
   */
  static void set_registry(const IVar& var, ICvarRegistry* registry) {
    var.registry_.store(registry, std::memory_order_release);
  }
};

namespace detail {

/** Renders a scalar default at compile time into a fixed buffer. */
struct DefaultText {
  std::array<char, 32> text{};
  u8 size = 0;
  constexpr std::string_view view() const { return {text.data(), size}; }
};

constexpr DefaultText RenderDefault(bool v) {
  DefaultText out;
  std::string_view s = v ? "true" : "false";
  for (char c : s)
    out.text[out.size++] = c;
  return out;
}

template <typename T>
  requires std::integral<T> && (!std::same_as<T, bool>)
constexpr DefaultText RenderDefault(T v) {
  DefaultText out;
  bool negative = v < 0;
  using U = std::make_unsigned_t<T>;
  U mag = negative ? U(0) - U(v) : U(v);
  std::array<char, 24> tmp{};
  i32 n = 0;
  do {
    tmp[n++] = char('0' + mag % 10);
    mag /= 10;
  } while (mag);
  if (negative)
    out.text[out.size++] = '-';
  while (n)
    out.text[out.size++] = tmp[--n];
  return out;
}

constexpr DefaultText RenderDefault(f64 v) {
  DefaultText out;
  bool negative = v < 0;
  if (negative)
    v = -v;
  auto whole = static_cast<u64>(v);
  f64 frac = v - static_cast<f64>(whole);
  auto whole_text = RenderDefault<u64>(whole);
  if (negative)
    out.text[out.size++] = '-';
  for (char c : whole_text.view())
    out.text[out.size++] = c;
  out.text[out.size++] = '.';
  for (i32 i = 0; i < 6; ++i) {
    frac *= 10.0;
    auto digit = static_cast<i32>(frac);
    out.text[out.size++] = char('0' + digit);
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
  constexpr StringCell() = default;
  constexpr StringCell(std::string_view s) {
    for (char c : s.substr(0, kCapacity))
      text[size++] = c;
  }
  constexpr std::string_view view() const { return {text.data(), size}; }
};

template <typename T>
constexpr Type TypeOf() {
  if constexpr (std::same_as<T, bool>)
    return Type::Bool;
  else if constexpr (std::same_as<T, i32>)
    return Type::Int32;
  else if constexpr (std::same_as<T, i64>)
    return Type::Int64;
  else if constexpr (std::same_as<T, u32>)
    return Type::UInt32;
  else if constexpr (std::same_as<T, u64>)
    return Type::UInt64;
  else if constexpr (std::same_as<T, f64>)
    return Type::Double;
  else
    return Type::String;
}

/**
 * Parses cvar text into a scalar. Bool accepts true, 1, yes, on and false, 0, no, off.
 *
 * @tparam T The value type.
 * @param text The value as text.
 * @return The parsed value, or std::nullopt when the text does not parse.
 */
template <typename T>
std::optional<T> ParseValue(std::string_view text);

template <typename... Args>
constexpr size_t CountArgs(Args&&...) {
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
class Var final : public IVar {
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
  constexpr Var(std::string_view name, Init default_value, std::string_view description)
      : default_text_(MakeDefaultText(default_value)),
        default_string_(MakeDefaultString(default_value)),
        cell_(MakeCell(default_value)) {
    desc_.name = name;
    desc_.scope = name.substr(0, name.find('.'));
    desc_.type = detail::TypeOf<T>();
    desc_.description = description;
  }
  constexpr Var(const Var&) = default;

  /**
   * Sets an inclusive numeric range.
   *
   * @param min_value The smallest value.
   * @param max_value The largest value.
   * @return This var with the range set.
   */
  constexpr Var range(f64 min_value, f64 max_value) && {
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
  constexpr Var allowed(std::span<const std::string_view> values) && {
    desc_.constraints.allowed = values;
    return *this;
  }
  /**
   * Sets when the value may change.
   *
   * @param lc The lifecycle.
   * @return This var with the lifecycle set.
   */
  constexpr Var lifecycle(Lifecycle lc) && {
    desc_.lifecycle = lc;
    return *this;
  }
  /**
   * Adds validation on the text form.
   *
   * @param fn Returns false for text the var rejects.
   * @return This var with the validator set.
   */
  constexpr Var validator(bool (*fn)(std::string_view)) && {
    desc_.constraints.validator = fn;
    return *this;
  }
  /**
   * Shows the cvar only in debug builds of the settings overlay.
   *
   * @return This var with Desc::debug_only set.
   */
  constexpr Var debug_only() && {
    desc_.debug_only = true;
    return *this;
  }

  /**
   * Marks the cvar as never written to a config file.
   *
   * @return This var with Desc::no_save set.
   */
  constexpr Var no_save() && {
    desc_.no_save = true;
    return *this;
  }

  /**
   * Returns the current value.
   *
   * @return The current value.
   */
  T get() const;
  /**
   * Sets through the registry with source Runtime.
   *
   * @param value The new value.
   * @return Ok, NotFound when the var is not registered, otherwise as ICvarRegistry::Set.
   */
  [[nodiscard]] Result<void> set(const T& value);

  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  const Desc& desc() const override { return desc_; }
  /**
   * Returns the default as text.
   *
   * @return The default text.
   */
  std::string_view DefaultText() const override {
    if constexpr (kIsString)
      return default_string_.view();
    else
      return default_text_.view();
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
    return Err(MakeError(Error::NotACommand, std::string(desc_.name) + " is not a command"));
  }

 private:
  static constexpr detail::DefaultText MakeDefaultText(Init v) {
    if constexpr (kIsString)
      return {};
    else
      return detail::RenderDefault(v);
  }
  static constexpr detail::StringCell MakeDefaultString(Init v) {
    if constexpr (kIsString)
      return detail::StringCell(v);
    else
      return {};
  }
  static constexpr Cell MakeCell(Init v) {
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

  Desc desc_;
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
class Command final : public IVar {
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
  constexpr Command(std::string_view name, Fn fn, std::string_view description) : fn_(fn) {
    desc_.name = name;
    desc_.scope = name.substr(0, name.find('.'));
    desc_.type = Type::Command;
    desc_.description = description;
  }
  /**
   * Returns the descriptor.
   *
   * @return The descriptor.
   */
  const Desc& desc() const override { return desc_; }
  /**
   * Returns the placeholder default text.
   *
   * @return The literal "<command>".
   */
  std::string_view DefaultText() const override { return "<command>"; }
  /**
   * Rejects the call, a command holds no value.
   *
   * @return NotAVar.
   */
  [[nodiscard]] Result<void> Parse(std::string_view) override {
    return Err(MakeError(Error::NotAVar, std::string(desc_.name) + " is a command"));
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
  Desc desc_;
  Fn fn_;
};

/**
 * Reports whether a var holds a value other than its default.
 *
 * @param var The var or command to inspect.
 * @return True when the var is registered and its value came from anywhere but the default.
 */
inline bool IsSet(const IVar& var) {
  auto* registry = var.registry();
  return registry && registry->Source(var.desc().name) != CvarSource::Default;
}

/**
 * Reads every REX_<SCOPE>_<LEAF> environment variable into config.cvars.values with source
 * Environment. The first underscore separates the scope from the leaf.
 *
 * @param config The configuration to fill.
 */
REX_API void ApplyEnvironment(rex::Config& config);

namespace detail {

template <typename... Vars>
constexpr std::array<const IVar*, sizeof...(Vars)> MakeTable(const Vars&... vars) {
  return {static_cast<const IVar*>(&vars)...};
}

template <>
inline std::optional<bool> ParseValue<bool>(std::string_view text) {
  if (text == "true" || text == "1" || text == "yes" || text == "on")
    return true;
  if (text == "false" || text == "0" || text == "no" || text == "off")
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
T Var<T>::get() const {
  if constexpr (kIsString) {
    std::atomic_ref<u8> lock(lock_);
    while (lock.exchange(1, std::memory_order_acquire)) {}
    std::string out(cell_.view());
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
Result<void> Var<T>::set(const T& value) {
  auto* reg = registry();
  if (!reg) {
    return Err(MakeError(Error::NotFound, std::string(desc_.name) + " is not registered"));
  }
  return reg->Set(desc_.name, Render(value));
}

template <typename T>
Result<void> Var<T>::Parse(std::string_view text) {
  auto parsed = detail::ParseValue<T>(text);
  if (!parsed) {
    return Err(MakeError(Error::Invalid,
                         std::string(desc_.name) + " cannot parse '" + std::string(text) + "'"));
  }
  Store(*parsed);
  return Ok();
}

template <typename T>
std::string Var<T>::Render() const {
  return Render(get());
}

}  // namespace rex::cvar

namespace rex {

/** 1 value arriving before Init. */
struct CvarOverride {
  /** Dotted cvar name such as gpu.vsync. */
  std::string name;
  /** The value as text. */
  std::string value;
  /** Source the registry records for the value. */
  cvar::CvarSource source = cvar::CvarSource::CommandLine;
};

/** A consumer table, registered before any module. */
struct CvarTable {
  /** Scope every cvar in the table belongs to. */
  std::string scope;
  /** The cvars in the table. */
  std::span<const cvar::IVar* const> vars;
};

/** Cvar tables, values and locks that Init applies in order. */
struct CvarConfig {
  /** Consumer tables, registered in order. */
  std::vector<CvarTable> tables;
  /** Values arriving before Init, applied in order. */
  std::vector<CvarOverride> values;
  /** Dotted names of the cvars Init locks. */
  std::vector<std::string> locked;
  /**
   * Appends a Default override, applied only while the cvar's source is Default.
   *
   * @param name Dotted cvar name.
   * @param value The value as text.
   */
  void SetDefault(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::Default});
  }
  /**
   * Appends a CommandLine override.
   *
   * @param name Dotted cvar name.
   * @param value The value as text.
   */
  void Set(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::CommandLine});
  }
  /**
   * Appends to locked.
   *
   * @param name Dotted cvar name.
   */
  void Lock(std::string_view name) { locked.emplace_back(name); }

  /**
   * Looks up the value Init would apply to a cvar, before Init runs.
   *
   * @param name Dotted cvar name, such as "gpu.vsync".
   * @return The queued value with the highest source precedence, or std::nullopt when no
   *         value is queued for name.
   */
  std::optional<std::string> Get(std::string_view name) const {
    const CvarOverride* best = nullptr;
    for (const CvarOverride& value : values) {
      if (value.name == name && (!best || value.source >= best->source)) {
        best = &value;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return best->value;
  }
};

}  // namespace rex

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
  inline ::rex::cvar::Var<bool> name =                        \
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
  inline ::rex::cvar::Var<::rex::i32> name =                   \
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
  inline ::rex::cvar::Var<::rex::i64> name =                   \
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
  inline ::rex::cvar::Var<::rex::u32> name =                    \
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
  inline ::rex::cvar::Var<::rex::u64> name =                    \
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
  inline ::rex::cvar::Var<::rex::f64> name =                    \
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
  inline ::rex::cvar::Var<std::string> name =                   \
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
  inline ::rex::cvar::Command name {                              \
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
  inline ::rex::cvar::Command name {                       \
    #scope "." #name, fn, description                      \
  }
/**
 * Builds static storage for an allowed value list, for use with .allowed().
 *
 * @param ... The allowed values as string literals.
 *
 * @code
 * .allowed(REXCVAR_ALLOWED("none", "fast", "some", "full"))
 * @endcode
 */
#define REXCVAR_ALLOWED(...)                                                                       \
  ([]() constexpr -> ::std::span<const ::std::string_view> {                                       \
    static constexpr ::std::array<::std::string_view, ::rex::cvar::detail::CountArgs(__VA_ARGS__)> \
        values{__VA_ARGS__};                                                                       \
    return values;                                                                                 \
  }())
/**
 * Declares a module's cvar table, the pointers Connect passes to ICvarRegistry::Register.
 *
 * @param table Name of the table variable.
 * @param ... The cvars in the table.
 *
 * @code
 * REXCVAR_TABLE(kGPUCvars, vsync, resolution_scale_x);
 * @endcode
 */
#define REXCVAR_TABLE(table, ...) \
  inline constexpr auto table = ::rex::cvar::detail::MakeTable(__VA_ARGS__)
/**
 * Reads a cvar.
 *
 * @param name The cvar variable.
 *
 * @code
 * bool vsync = REXCVAR_GET(vsync);
 * @endcode
 */
#define REXCVAR_GET(name) ((name).get())
/**
 * Sets a cvar through the registry with source Runtime.
 *
 * @param name The cvar variable.
 * @param value The new value.
 *
 * @code
 * REXCVAR_SET(vsync, false);
 * @endcode
 */
#define REXCVAR_SET(name, value) ((name).set(value))
