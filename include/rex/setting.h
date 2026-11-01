/**
 * @file        setting.h
 * @brief       Typed settings and the owners that declare them
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {

class Command;
class SettingOwner;
class Settings;

enum class SettingType : u8 {
  kBool,
  kI32,
  kI64,
  kU32,
  kU64,
  kF64,
  kString,
  kPath, /**< std::filesystem::path. A value may name other paths as {scope.name}. */
};

/** Where a setting's value came from, lowest first. A higher layer wins. kLock refuses writes. */
enum class SettingLayer : u8 {
  kDefault,
  kTitle,       /**< The title's code. Writable until the title launches. */
  kFile,        /**< The player's config file. */
  kEnvironment, /**< A REX_<SCOPE>_<NAME> environment variable. */
  kCommandLine, /**< A --scope.name option. */
  kRuntime,     /**< The console, the settings overlay or code after startup. */
  kLock,        /**< Locked by the title's code. */
};

enum class SettingLife : u8 {
  kLive,        /**< Any time. Readers see the change on their next read. */
  kRestart,     /**< Any time, applied on the next start. The registry lists it as pending. */
  kStartupOnly, /**< Until the providers connect. */
  kUntilLaunch,
};

struct SettingConstraints {
  std::optional<f64> min;
  std::optional<f64> max;
  std::vector<std::string> allowed; /**< The only accepted texts, when not empty. */
  SettingLife life = SettingLife::kLive;
  bool no_save = false;    /**< Never written to the config file. */
  bool debug_only = false; /**< Hidden from the settings overlay outside debug builds. */
  bool key_chord = false;  /**< A key combination the settings overlay captures. */
};

/** Trait for REX_SETTING that sets the numeric range. */
struct Range {
  f64 min;
  f64 max;
};

/** Trait for REX_SETTING that sets the only accepted texts. */
struct Allowed {
  std::vector<std::string> values;

  template <typename... Texts>
  explicit Allowed(Texts... texts) : values{std::string(texts)...} {}
};

inline constexpr SettingLife kRestart = SettingLife::kRestart;
inline constexpr SettingLife kStartupOnly = SettingLife::kStartupOnly;
inline constexpr SettingLife kUntilLaunch = SettingLife::kUntilLaunch;
inline constexpr struct NoSave {
} kNoSave;
inline constexpr struct DebugOnly {
} kDebugOnly;
inline constexpr struct KeyChord {
} kKeyChord;

/** The untyped part of a setting, which the registry, the console and the overlay use. */
class REX_API SettingBase : public NonCopyable {
 public:
  virtual ~SettingBase();

  /** Returns the full name, the owner's scope and the member name, such as "apu.mute". */
  const std::string& GetName() const;

  const std::string& GetHelp() const;

  SettingType GetType() const;

  /**
   * Returns what the setting accepts.
   */
  const SettingConstraints& GetConstraints() const;

  SettingOwner& GetOwner() const;

  /**
   * Returns the layer the current value came from.
   */
  SettingLayer GetLayer() const;

  /** Reports whether the value can no longer change, because it is locked or past its life. */
  bool IsFrozen() const;

  /** Reports whether a kRestart setting changed and waits for the next start. */
  bool IsRestartPending() const;

  /** A path keeps its {scope.name} references. */
  std::string GetText() const;

  std::string GetDefaultText() const;

  /**
   * Checks a text against the type and the constraints without storing it.
   *
   * @return Ok, or an error naming the setting and the reason.
   */
  [[nodiscard]] Result<void> Check(std::string_view text) const;

  /**
   * Stores a text at a layer.
   *
   * @return Ok, or an error when the text fails the check, the setting is frozen, or a higher layer
   * holds the value.
   */
  [[nodiscard]] Result<void> SetText(SettingLayer layer, std::string_view text);

  /**
   * Returns the setting to its default and clears its layer. It fails when the setting is frozen.
   */
  [[nodiscard]] Result<void> Reset();

 protected:
  SettingBase(SettingOwner& owner, std::string_view name, SettingType type, std::string_view help);

  /**
   * Returns the constraints for a derived class to fill from its traits.
   */
  SettingConstraints& EditConstraints();

  /**
   * Stores a text written by code, in the title layer before launch and the runtime layer after.
   *
   * @return Ok, or the same errors as SetText.
   */
  [[nodiscard]] Result<void> SetFromCode(std::string_view text);

 private:
  friend class SettingOwner;
  friend class Settings;

  virtual bool Store(std::string_view text) = 0;
  virtual void Publish() = 0;
  virtual std::string Render() const = 0;
  virtual std::string RenderDefault() const = 0;
  virtual void Notify() = 0;

  struct State;
  std::unique_ptr<State> state_;
};

/**
 * A typed setting, declared with REX_SETTING inside its owner. Scalars read lock free.
 *
 * @tparam T bool, i32, i64, u32, u64, f64, std::string or std::filesystem::path.
 */
template <typename T>
class Setting final : public SettingBase {
 public:
  /**
   * Declares the setting. REX_SETTING is the usual way to write it.
   *
   * @tparam Traits The traits, such as Range, Allowed, kRestart or kNoSave.
   */
  template <typename... Traits>
  Setting(SettingOwner* owner, std::string_view name, T default_value, std::string_view help,
          Traits... traits);

  /** A path comes back with its {scope.name} references resolved. */
  T Get() const;

  /**
   * Stores a value at the title layer before launch, the runtime layer after.
   *
   * @return Ok, or the same errors as SetText.
   */
  [[nodiscard]] Result<void> Set(const T& value);

  /**
   * Stores a value at the lock layer, so no later layer changes it.
   *
   * @return Ok, or an error when the setting is frozen.
   */
  [[nodiscard]] Result<void> Lock(const T& value);

  /**
   * Calls a function after every change, on the thread that made it.
   *
   * @param fn Called with the new value.
   * @return The guard. The calls stop when it is destroyed.
   */
  [[nodiscard]] ScopeGuard OnChange(MoveOnlyFunction<void(const T&)> fn);

 private:
  static constexpr bool kScalar =
      !std::is_same_v<T, std::string> && !std::is_same_v<T, std::filesystem::path>;
  using Storage = std::conditional_t<kScalar, std::atomic<T>, T>;

  bool Store(std::string_view text) override;
  void Publish() override;
  std::string Render() const override;
  std::string RenderDefault() const override;
  void Notify() override;

  T GetActive() const;

  T default_;
  T staged_;
  Storage active_;
  mutable std::mutex mutex_;
  u64 next_watch_ = 1;
  std::vector<std::pair<u64, std::shared_ptr<MoveOnlyFunction<void(const T&)>>>> watches_;
};

/** A class whose members include settings. Its scope prefixes every member name. */
class REX_API SettingOwner : public NonCopyable {
 public:
  /**
   * @param scope The scope, such as "apu" or "gpu.xenos".
   */
  explicit SettingOwner(std::string_view scope);

  /** Removes the owner and its settings from the registry. */
  virtual ~SettingOwner();

  const std::string& GetScope() const;

  /**
   * Returns the settings declared in this owner, in declaration order.
   */
  std::span<SettingBase* const> GetOwnedSettings() const;

 private:
  friend class SettingBase;
  friend class Command;
  friend class Settings;
  struct State;
  std::unique_ptr<State> state_;
};

namespace detail {

REX_API bool ParseSetting(std::string_view text, bool& out);
REX_API bool ParseSetting(std::string_view text, i32& out);
REX_API bool ParseSetting(std::string_view text, i64& out);
REX_API bool ParseSetting(std::string_view text, u32& out);
REX_API bool ParseSetting(std::string_view text, u64& out);
REX_API bool ParseSetting(std::string_view text, f64& out);
REX_API bool ParseSetting(std::string_view text, std::string& out);
REX_API bool ParseSetting(std::string_view text, std::filesystem::path& out);

REX_API std::string RenderSetting(bool value);
REX_API std::string RenderSetting(i32 value);
REX_API std::string RenderSetting(i64 value);
REX_API std::string RenderSetting(u32 value);
REX_API std::string RenderSetting(u64 value);
REX_API std::string RenderSetting(f64 value);
REX_API std::string RenderSetting(const std::string& value);
REX_API std::string RenderSetting(const std::filesystem::path& value);

REX_API std::filesystem::path ResolveSettingPath(const std::filesystem::path& value);

template <typename T>
constexpr SettingType TypeOf() {
  if constexpr (std::is_same_v<T, bool>)
    return SettingType::kBool;
  else if constexpr (std::is_same_v<T, i32>)
    return SettingType::kI32;
  else if constexpr (std::is_same_v<T, i64>)
    return SettingType::kI64;
  else if constexpr (std::is_same_v<T, u32>)
    return SettingType::kU32;
  else if constexpr (std::is_same_v<T, u64>)
    return SettingType::kU64;
  else if constexpr (std::is_same_v<T, f64>)
    return SettingType::kF64;
  else if constexpr (std::is_same_v<T, std::string>)
    return SettingType::kString;
  else
    return SettingType::kPath;
}

inline void ApplyTrait(SettingConstraints& c, const Range& r) {
  c.min = r.min;
  c.max = r.max;
}
inline void ApplyTrait(SettingConstraints& c, const Allowed& a) {
  c.allowed = a.values;
}
inline void ApplyTrait(SettingConstraints& c, SettingLife life) {
  c.life = life;
}
inline void ApplyTrait(SettingConstraints& c, NoSave) {
  c.no_save = true;
}
inline void ApplyTrait(SettingConstraints& c, DebugOnly) {
  c.debug_only = true;
}
inline void ApplyTrait(SettingConstraints& c, KeyChord) {
  c.key_chord = true;
}

}  // namespace detail

template <typename T>
template <typename... Traits>
Setting<T>::Setting(SettingOwner* owner, std::string_view name, T default_value,
                    std::string_view help, Traits... traits)
    : SettingBase(*owner, name, detail::TypeOf<T>(), help),
      default_(default_value),
      staged_(default_value),
      active_(default_value) {
  (detail::ApplyTrait(EditConstraints(), traits), ...);
}

template <typename T>
T Setting<T>::GetActive() const {
  if constexpr (kScalar) {
    return active_.load(std::memory_order_relaxed);
  } else {
    std::lock_guard lock(mutex_);
    return active_;
  }
}

template <typename T>
T Setting<T>::Get() const {
  if constexpr (std::is_same_v<T, std::filesystem::path>) {
    return detail::ResolveSettingPath(GetActive());
  } else {
    return GetActive();
  }
}

template <typename T>
Result<void> Setting<T>::Set(const T& value) {
  return SetFromCode(detail::RenderSetting(value));
}

template <typename T>
Result<void> Setting<T>::Lock(const T& value) {
  return SetText(SettingLayer::kLock, detail::RenderSetting(value));
}

template <typename T>
ScopeGuard Setting<T>::OnChange(MoveOnlyFunction<void(const T&)> fn) {
  u64 id;
  {
    std::lock_guard lock(mutex_);
    id = next_watch_++;
    watches_.emplace_back(id, std::make_shared<MoveOnlyFunction<void(const T&)>>(std::move(fn)));
  }
  return ScopeGuard([this, id] {
    std::lock_guard lock(mutex_);
    std::erase_if(watches_, [id](const auto& entry) { return entry.first == id; });
  });
}

template <typename T>
bool Setting<T>::Store(std::string_view text) {
  T parsed{};
  if (!detail::ParseSetting(text, parsed)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  staged_ = std::move(parsed);
  return true;
}

template <typename T>
void Setting<T>::Publish() {
  std::lock_guard lock(mutex_);
  if constexpr (kScalar) {
    active_.store(staged_, std::memory_order_relaxed);
  } else {
    active_ = staged_;
  }
}

template <typename T>
std::string Setting<T>::Render() const {
  std::lock_guard lock(mutex_);
  return detail::RenderSetting(staged_);
}

template <typename T>
std::string Setting<T>::RenderDefault() const {
  return detail::RenderSetting(default_);
}

template <typename T>
void Setting<T>::Notify() {
  std::vector<std::shared_ptr<MoveOnlyFunction<void(const T&)>>> calls;
  T value = GetActive();
  {
    std::lock_guard lock(mutex_);
    for (auto& entry : watches_) {
      calls.push_back(entry.second);
    }
  }
  for (auto& call : calls) {
    (*call)(value);
  }
}
}  // namespace rex

/**
 * Declares a setting inside a SettingOwner.
 *
 * @code
 * REX_SETTING(i32, count, 4, "How many", Range{1, 8}, kRestart);
 * @endcode
 *
 * @param name The member name. The full name is the owner's scope plus this.
 * @param default_value A path default may name other paths as {scope.name}.
 * @param ... Optional traits such as Range, Allowed, kRestart, kStartupOnly and kUntilLaunch.
 */
#define REX_SETTING(Type, name, default_value, help, ...)       \
  ::rex::Setting<Type> name {                                   \
    this, #name, default_value, help __VA_OPT__(, ) __VA_ARGS__ \
  }
