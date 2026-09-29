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

#include <rex/cvar/registry.h>
#include <rex/result.h>

namespace rex {
struct Config;
}

namespace rex::cvar {

/** Value type tag. */
enum class Type : uint8_t { Bool, Int32, Int64, UInt32, UInt64, Double, String, Command };

/** When a cvar may change. */
enum class Lifecycle : uint8_t { HotReload, InitOnly, RequiresRestart };

/** Where a cvar value came from, ascending precedence. */
enum class CvarSource : uint8_t { Default, Config, Environment, CommandLine, Runtime };

/** Error codes under ErrorCategory::Config. */
enum class Error : int32_t {
  NotFound = 1,
  Invalid,
  InitOnly,
  Locked,
  DuplicateName,
  DuplicateScope,
  BadName,
  NotACommand,
  NotAVar,
};

/**
 * Builds a Config category error with a cvar error code.
 */
inline rex::Error MakeError(Error code, std::string message) {
  return rex::Error(ErrorCategory::Config, std::move(message), static_cast<int>(code));
}

/** Value constraints. Constant data. */
struct Constraints {
  std::optional<double> min;
  std::optional<double> max;
  std::span<const std::string_view> allowed;
  /** Extra check on the text form, or null. */
  bool (*validator)(std::string_view) = nullptr;
};

/** Everything the registry and the console know about 1 cvar. */
struct Desc {
  std::string_view name;
  std::string_view scope;
  Type type = Type::Bool;
  std::string_view description;
  Lifecycle lifecycle = Lifecycle::HotReload;
  Constraints constraints;
  bool debug_only = false;
};

class detail_access;

/**
 * Type erased view of a Var<T> or Command. Registered by pointer, never copied.
 */
class IVar {
 public:
  constexpr IVar() = default;
  /** Copies nothing but the vtable. Chain methods copy a Var before any registration. */
  constexpr IVar(const IVar&) {}
  IVar& operator=(const IVar&) = delete;
  virtual ~IVar() = default;
  /** @return The descriptor. */
  virtual const Desc& desc() const = 0;
  /** @return The default as text, from storage that lives as long as the var. */
  virtual std::string_view DefaultText() const = 0;
  /**
   * Parses and stores. Fails with Invalid when the text does not parse. Constraints are the
   * registry's job.
   */
  virtual Status Parse(std::string_view text) = 0;
  /** @return The current value as text. */
  virtual std::string Render() const = 0;
  /** Commands only. Fails with NotACommand on a var. */
  virtual Status Invoke(std::string_view args) = 0;
  /**
   * The registry that holds this var, or null. Set by Register, cleared by Unregister.
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
  /** Records the registry that holds var, or null. */
  static void set_registry(const IVar& var, ICvarRegistry* registry) {
    var.registry_.store(registry, std::memory_order_release);
  }
};

namespace detail {

/** Renders a scalar default at compile time into a fixed buffer. */
struct DefaultText {
  std::array<char, 32> text{};
  uint8_t size = 0;
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
  int n = 0;
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

constexpr DefaultText RenderDefault(double v) {
  DefaultText out;
  bool negative = v < 0;
  if (negative)
    v = -v;
  auto whole = static_cast<uint64_t>(v);
  double frac = v - static_cast<double>(whole);
  auto whole_text = RenderDefault<uint64_t>(whole);
  if (negative)
    out.text[out.size++] = '-';
  for (char c : whole_text.view())
    out.text[out.size++] = c;
  out.text[out.size++] = '.';
  for (int i = 0; i < 6; ++i) {
    frac *= 10.0;
    auto digit = static_cast<int>(frac);
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
  uint8_t size = 0;
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
  else if constexpr (std::same_as<T, int32_t>)
    return Type::Int32;
  else if constexpr (std::same_as<T, int64_t>)
    return Type::Int64;
  else if constexpr (std::same_as<T, uint32_t>)
    return Type::UInt32;
  else if constexpr (std::same_as<T, uint64_t>)
    return Type::UInt64;
  else if constexpr (std::same_as<T, double>)
    return Type::Double;
  else
    return Type::String;
}

/** Parses cvar text into a scalar. Bool accepts true, 1, yes, on and false, 0, no, off. */
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
 * Usage: REXCVAR_BOOL(gpu, vsync, true, "Wait for vertical blank before presenting");
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

  /** Numeric range, inclusive. */
  constexpr Var range(double min_value, double max_value) && {
    desc_.constraints.min = min_value;
    desc_.constraints.max = max_value;
    return *this;
  }
  /** Allowed values. The span must have static storage, REXCVAR_ALLOWED provides it. */
  constexpr Var allowed(std::span<const std::string_view> values) && {
    desc_.constraints.allowed = values;
    return *this;
  }
  /** When the value may change. */
  constexpr Var lifecycle(Lifecycle lc) && {
    desc_.lifecycle = lc;
    return *this;
  }
  /** Extra validation on the text form. */
  constexpr Var validator(bool (*fn)(std::string_view)) && {
    desc_.constraints.validator = fn;
    return *this;
  }
  /** Shown only in debug builds of the settings overlay. */
  constexpr Var debug_only() && {
    desc_.debug_only = true;
    return *this;
  }

  /** @return The current value. */
  T get() const;
  /**
   * Sets through the registry with source Runtime. Fails with NotFound when the var is not
   * registered, otherwise as ICvarRegistry::Set.
   */
  Status set(const T& value);

  /** @return The descriptor. */
  const Desc& desc() const override { return desc_; }
  /** @return The default as text. */
  std::string_view DefaultText() const override {
    if constexpr (kIsString)
      return default_string_.view();
    else
      return default_text_.view();
  }
  /** Parses text into the cell. Fails with Invalid. */
  Status Parse(std::string_view text) override;
  /** @return The value as text. */
  std::string Render() const override;
  /** Fails with NotACommand. */
  Status Invoke(std::string_view) override {
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
    else if constexpr (std::same_as<T, double>)
      return std::format("{}", value);
    else
      return std::to_string(value);
  }
  void Store(const T& value);

  Desc desc_;
  detail::DefaultText default_text_;
  detail::StringCell default_string_;
  mutable Cell cell_;
  mutable uint8_t lock_ = 0;
};

/**
 * A console command. Constant initialized.
 *
 * Usage: REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Print the arguments");
 */
class Command final : public IVar {
 public:
  /** The command function. */
  using Fn = void (*)(std::string_view args);
  /**
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
  /** @return The descriptor. */
  const Desc& desc() const override { return desc_; }
  /** @return The literal "<command>". */
  std::string_view DefaultText() const override { return "<command>"; }
  /** Fails with NotAVar. */
  Status Parse(std::string_view) override {
    return Err(MakeError(Error::NotAVar, std::string(desc_.name) + " is a command"));
  }
  /** @return The literal "<command>". */
  std::string Render() const override { return "<command>"; }
  /** Runs the command. */
  Status Invoke(std::string_view args) override {
    fn_(args);
    return Ok();
  }

 private:
  Desc desc_;
  Fn fn_;
};

/**
 * @return True when the var is registered and its value came from anywhere but the default.
 */
inline bool IsSet(const IVar& var) {
  auto* registry = var.registry();
  return registry && registry->Source(var.desc().name) != CvarSource::Default;
}

/**
 * Reads every REX_<SCOPE>_<LEAF> environment variable into config.cvars.values with source
 * Environment. The first underscore separates the scope from the leaf.
 */
void ApplyEnvironment(rex::Config& config);

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
  if constexpr (std::same_as<T, double>) {
    double v = 0;
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
    std::atomic_ref<uint8_t> lock(lock_);
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
    std::atomic_ref<uint8_t> lock(lock_);
    while (lock.exchange(1, std::memory_order_acquire)) {}
    cell_ = detail::StringCell(value);
    lock.store(0, std::memory_order_release);
  } else {
    std::atomic_ref<Cell>(cell_).store(value, std::memory_order_relaxed);
  }
}

template <typename T>
Status Var<T>::set(const T& value) {
  auto* reg = registry();
  if (!reg) {
    return Err(MakeError(Error::NotFound, std::string(desc_.name) + " is not registered"));
  }
  return reg->Set(desc_.name, Render(value));
}

template <typename T>
Status Var<T>::Parse(std::string_view text) {
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
  std::string name;
  std::string value;
  cvar::CvarSource source = cvar::CvarSource::CommandLine;
};

/** A consumer table, registered before any module. */
struct CvarTable {
  std::string scope;
  std::span<const cvar::IVar* const> vars;
};

/** Cvar tables, values and locks that Init applies in order. */
struct CvarConfig {
  std::vector<CvarTable> tables;
  std::vector<CvarOverride> values;
  std::vector<std::string> locked;
  /** Appends a Default override, applied only while the cvar's source is Default. */
  void SetDefault(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::Default});
  }
  /** Appends a CommandLine override. */
  void Set(std::string_view name, std::string_view value) {
    values.push_back({std::string(name), std::string(value), cvar::CvarSource::CommandLine});
  }
  /** Appends to locked. */
  void Lock(std::string_view name) { locked.emplace_back(name); }
};

}  // namespace rex

/**
 * Declares a bool cvar. Expands to
 *   inline ::rex::cvar::Var<bool> name = ::rex::cvar::Var<bool>("scope.name", default_value,
 * description) Usage: REXCVAR_BOOL(gpu, vsync, true, "Wait for vertical blank before presenting");
 */
#define REXCVAR_BOOL(scope, name, default_value, description) \
  inline ::rex::cvar::Var<bool> name =                        \
      ::rex::cvar::Var<bool>(#scope "." #name, default_value, description)
/** Declares an int32_t cvar. See REXCVAR_BOOL. */
#define REXCVAR_INT32(scope, name, default_value, description) \
  inline ::rex::cvar::Var<int32_t> name =                      \
      ::rex::cvar::Var<int32_t>(#scope "." #name, default_value, description)
/** Declares an int64_t cvar. See REXCVAR_BOOL. */
#define REXCVAR_INT64(scope, name, default_value, description) \
  inline ::rex::cvar::Var<int64_t> name =                      \
      ::rex::cvar::Var<int64_t>(#scope "." #name, default_value, description)
/** Declares a uint32_t cvar. See REXCVAR_BOOL. */
#define REXCVAR_UINT32(scope, name, default_value, description) \
  inline ::rex::cvar::Var<uint32_t> name =                      \
      ::rex::cvar::Var<uint32_t>(#scope "." #name, default_value, description)
/** Declares a uint64_t cvar. See REXCVAR_BOOL. */
#define REXCVAR_UINT64(scope, name, default_value, description) \
  inline ::rex::cvar::Var<uint64_t> name =                      \
      ::rex::cvar::Var<uint64_t>(#scope "." #name, default_value, description)
/** Declares a double cvar. See REXCVAR_BOOL. */
#define REXCVAR_DOUBLE(scope, name, default_value, description) \
  inline ::rex::cvar::Var<double> name =                        \
      ::rex::cvar::Var<double>(#scope "." #name, default_value, description)
/** Declares a string cvar, at most 255 bytes. See REXCVAR_BOOL. */
#define REXCVAR_STRING(scope, name, default_value, description) \
  inline ::rex::cvar::Var<std::string> name =                   \
      ::rex::cvar::Var<std::string>(#scope "." #name, default_value, description)
/**
 * Declares a console command without arguments. fn is void().
 * Usage: REXCVAR_COMMAND(console, clear, ConsoleClear, "Clear the console");
 */
#define REXCVAR_COMMAND(scope, name, fn, description)             \
  inline ::rex::cvar::Command name {                              \
    #scope "." #name, [](std::string_view) { fn(); }, description \
  }
/**
 * Declares a console command taking the argument text. fn is void(std::string_view).
 * Usage: REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Print the arguments");
 */
#define REXCVAR_COMMAND_ARGS(scope, name, fn, description) \
  inline ::rex::cvar::Command name {                       \
    #scope "." #name, fn, description                      \
  }
/**
 * Static storage for an allowed value list, for use with .allowed().
 * Usage: .allowed(REXCVAR_ALLOWED("none", "fast", "some", "full"))
 */
#define REXCVAR_ALLOWED(...)                                                                       \
  ([]() constexpr -> ::std::span<const ::std::string_view> {                                       \
    static constexpr ::std::array<::std::string_view, ::rex::cvar::detail::CountArgs(__VA_ARGS__)> \
        values{__VA_ARGS__};                                                                       \
    return values;                                                                                 \
  }())
/**
 * A module's cvar table, the pointers Connect passes to ICvarRegistry::Register.
 * Usage: REXCVAR_TABLE(kGPUCvars, vsync, resolution_scale_x);
 * Expands to inline constexpr std::array<const IVar*, N> kGPUCvars = {&vsync, &resolution_scale_x};
 */
#define REXCVAR_TABLE(table, ...) \
  inline constexpr auto table = ::rex::cvar::detail::MakeTable(__VA_ARGS__)
/** Reads a cvar. Expands to (name.get()). */
#define REXCVAR_GET(name) (name.get())
/** Sets a cvar through the registry with source Runtime. Expands to (name.set(value)). */
#define REXCVAR_SET(name, value) (name.set(value))
