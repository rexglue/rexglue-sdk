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
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {

class ConsoleCommand;
class SettingBase;
class SettingOwner;
class SettingsRegistry;

template <typename T>
class Setting;

namespace detail {

/** One function watching one or more settings, called with their current values. */
class REX_API SettingWatch : public NonCopyable {
 public:
  virtual ~SettingWatch();

  /**
   * Adds the watch to each of its settings, in registration order after their other watches.
   *
   * @param self The watch, owned by the caller.
   */
  static void Link(const std::shared_ptr<SettingWatch>& self);

  /**
   * Returns the guard that removes the watch.
   *
   * @param self The watch.
   */
  [[nodiscard]] static ScopeGuard Guard(std::shared_ptr<SettingWatch> self);

  /**
   * Calls the function when a watched setting changed since the values of its last call. When a
   * call runs on another thread, it waits for that call and for at most one more call, which it
   * makes itself when that call is handed to it.
   */
  void Deliver();

  /**
   * Stops every later call, and waits for a call in progress on another thread. Inside the
   * watch's own call it does not wait.
   */
  void Remove();

  /**
   * Stops every later call because a watched setting is destroyed.
   *
   * @param setting The setting being destroyed.
   */
  void Detach(const SettingBase& setting);

  /** Returns the watched settings, in the order the function receives their values. */
  std::span<SettingBase* const> GetSettings() const;

 protected:
  explicit SettingWatch(std::vector<SettingBase*> settings);

  /**
   * Keeps the generation a call read from a setting.
   *
   * @param index The position of the setting in the watch.
   * @param generation The generation of the value the call receives.
   */
  void MarkSeen(size_t index, u64 generation);

  /**
   * Holds the call of a watch while it is linked and its first values are read, without calling
   * it. A write that arrives meanwhile waits, and when the hold ends it is handed the next call,
   * which calls only when a generation is newer than the one marked seen. No user code runs during
   * the hold, so it holds no lock rank.
   */
  class REX_API Hold : public NonCopyable {
   public:
    explicit Hold(SettingWatch& watch);
    ~Hold();

   private:
    SettingWatch& watch_;
    u64 ticket_;
  };

 private:
  /** Reads the current values, passes each generation to MarkSeen, and calls the function. */
  virtual void Call() = 0;

  void Unlink(const SettingBase* skip);

  /** True when a watched setting has a generation the last call did not read. Under mutex_. */
  bool HasNewer() const;

  /**
   * Makes the one call of the calling thread, which serves every ticket issued before it starts,
   * then hands the next call to the oldest waiting ticket or marks the watch idle. After a removal
   * it makes no call, serves every ticket and marks the watch idle.
   *
   * @param lock The held lock of mutex_, released on return.
   * @param ticket The ticket of the calling thread, which holds the call.
   */
  void Run(std::unique_lock<std::mutex>& lock, u64 ticket);

  /**
   * Waits without a lock until a call serves the ticket or the next call is handed to it.
   *
   * @param ticket The ticket of the calling thread.
   * @return True when the calling thread makes the next call.
   */
  bool WaitTurn(u64 ticket) const;

  /**
   * Ends the hold of the call: hands the next call to the oldest waiting ticket, or marks the
   * watch idle and serves every ticket.
   *
   * @param lock The held lock of mutex_, released on return.
   * @param own The ticket of the calling thread.
   * @param served The last ticket the ended hold serves.
   */
  void HandOn(std::unique_lock<std::mutex>& lock, u64 own, u64 served);

  std::mutex mutex_;     /**< Unranked, it guards running_ and requests_, takes no other lock and
                               calls no other code. */
  bool running_ = false; /**< A thread holds the call, from its ticket until it hands it on. */
  u64 requests_ = 0;     /**< The last ticket issued. */
  std::vector<SettingBase*> settings_;
  std::vector<u64> seen_;       /**< Written only by the thread that holds the call. */
  std::atomic<u64> served_{0};  /**< The last ticket a finished call serves. */
  std::atomic<u64> handoff_{0}; /**< The ticket the next call is handed to. */
  std::atomic<bool> removed_{false};
  std::atomic<bool> unlinked_{false};
  std::atomic<const void*> runner_{nullptr}; /**< The tag of the thread that holds the call. */
};

template <typename... T>
class SettingGroupWatch;

template <typename T>
class SettingListWatch;

/**
 * Adds a watch to its settings and calls it with the current values.
 *
 * @param watch The watch.
 * @return The guard that removes it.
 */
[[nodiscard]] REX_API ScopeGuard AddWatch(std::shared_ptr<SettingWatch> watch);

/** True when the calling thread runs a change function of a setting. */
REX_API bool IsInSettingChange();

}  // namespace detail

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
  kTitle,       /**< Code before the title launches, OnInit included. */
  kFile,        /**< The player's config file. */
  kEnvironment, /**< A REX_<SCOPE>_<NAME> environment variable. */
  kCommandLine, /**< A --scope.name option. */
  kRuntime,     /**< The console, the settings overlay, or code from launch until unload. */
  kLock,        /**< Locked by the project's code. */
};

enum class SettingMode : u8 {
  kNormal,          /**< Any time. Readers see the change on their next read. */
  kRequiresRestart, /**< Any time, applied on the next start. The registry lists it as pending. */
  kInitOnly,        /**< Until the modules init, just before OnInit. */
  kLockedInGame,    /**< Locked while a title runs, from its launch until it unloads. */
};

struct SettingConstraints {
  std::optional<f64> min;
  std::optional<f64> max;
  std::vector<std::string> allowed; /**< The only accepted texts, when not empty. */
  SettingMode mode = SettingMode::kNormal;
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

inline constexpr SettingMode kRequiresRestart = SettingMode::kRequiresRestart;
inline constexpr SettingMode kInitOnly = SettingMode::kInitOnly;
inline constexpr SettingMode kLockedInGame = SettingMode::kLockedInGame;
inline constexpr struct NoSave {
} kNoSave;
inline constexpr struct DebugOnly {
} kDebugOnly;
inline constexpr struct KeyChordTrait {
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

  /** Reports whether the value can no longer change, because it is locked or its mode has closed
   * it. */
  bool IsFrozen() const;

  /** Reports whether a kRequiresRestart setting changed and waits for the next start. */
  bool IsRestartPending() const;

  /**
   * Returns a count that rises each time the value changes. A cache built from the value keeps the
   * count it was built from, and rebuilds when this is higher. Any thread.
   */
  u64 GetGeneration() const;

  /** Returns the value as text. A path keeps its {scope.name} references. */
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
  /** What a commit stores. */
  enum class CommitKind : u8 {
    kWrite, /**< A text at a layer, refused when frozen, locked or held by a higher layer. */
    kReset, /**< The default, refused when frozen. */
    kEnd,   /**< The default at the end of a session, never refused and always published. */
  };

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

  /**
   * Refuses a commit that the layer, the lock or the frozen state rules out. The commit calls it
   * under the value lock.
   *
   * @return Ok, or the refusal naming the setting.
   */
  [[nodiscard]] Result<void> Admit(SettingLayer layer, CommitKind kind) const;

  /**
   * Sets the layer and the restart flag of a stored value. Called under the value lock.
   *
   * @param changed True when the stored text differs from the one before.
   * @return True when the value is published and its watches are called.
   */
  bool FinishCommit(SettingLayer layer, CommitKind kind, bool changed);

  /** Raises the generation of a published value. Called under the value lock. */
  void AdvanceGeneration();

  /**
   * Returns the error for a text that does not parse.
   *
   * @return The error naming the setting.
   */
  [[nodiscard]] Result<void> StoreFailed(std::string_view text, CommitKind kind) const;

  /** Calls the watches of the setting, or queues them while this thread runs a change call. */
  void Notify();

  /** Adds a watch after the other watches of the setting. Takes watches_mutex. */
  void LinkWatch(std::shared_ptr<detail::SettingWatch> watch);

 private:
  friend class SettingOwner;
  friend class SettingsRegistry;
  friend class detail::SettingWatch;

  /**
   * Parses a text, then admits and stores it under the value lock, then calls the watches.
   *
   * @return Ok, or the refusal naming the setting.
   */
  [[nodiscard]] virtual Result<void> Commit(SettingLayer layer, std::string_view text,
                                            CommitKind kind) = 0;
  virtual std::string Render() const = 0;
  virtual std::string RenderDefault() const = 0;

  /** Returns once a commit in progress on another thread has stored. */
  virtual void WaitForCommit() const = 0;

  void UnlinkWatch(const detail::SettingWatch* watch);

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
   * @tparam Traits The traits, such as Range, Allowed, kRequiresRestart or kNoSave.
   */
  template <typename... Traits>
  Setting(SettingOwner* owner, std::string_view name, const T& default_value, std::string_view help,
          Traits... traits);

  /** Returns the value. A path comes back with its {scope.name} references resolved. */
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
   * Calls a function after each change of the value, never when OnChange is called. The watch is
   * linked under the value lock, so a write that stores after that is a change that reaches the
   * function. A call after a change runs on the writing thread before the write returns. The
   * functions of a setting run in the order they were added, and a function never runs
   * concurrently with itself. A write made inside a change function stores immediately, and its
   * calls run after that function returns. A change function may write settings and post work to
   * another thread, and must not wait for another thread.
   *
   * @code
   * auto guard = perf.depth.OnChange([](const i32& value) { Resize(value); });
   * @endcode
   *
   * @param fn Called with the new value after each change.
   * @return The guard. Destroying it waits for a call in progress on another thread, and the
   * function is never called after that. Destroyed inside the function's own call, it does not
   * wait.
   */
  [[nodiscard]] ScopeGuard OnChange(MoveOnlyFunction<void(const T&)> fn);

  /**
   * Calls a function with the current value before Bind returns, and again after each change. The
   * rules of OnChange apply to every call. Bound inside a change function, the first call runs
   * after that function returns.
   *
   * @param fn Called with the current value.
   * @return The guard, as for OnChange.
   */
  [[nodiscard]] ScopeGuard Bind(MoveOnlyFunction<void(const T&)> fn);

 private:
  template <typename... U>
  friend class detail::SettingGroupWatch;
  template <typename U>
  friend class detail::SettingListWatch;

  static constexpr bool kScalar =
      !std::is_same_v<T, std::string> && !std::is_same_v<T, std::filesystem::path>;
  using Storage = std::conditional_t<kScalar, std::atomic<T>, T>;

  [[nodiscard]] Result<void> Commit(SettingLayer layer, std::string_view text,
                                    CommitKind kind) override;
  std::string Render() const override;
  std::string RenderDefault() const override;
  void WaitForCommit() const override;

  T GetActive() const;

  /** Returns the published value and its generation, read together. */
  T Snapshot(u64& generation) const;

  /**
   * Returns the published value and its generation, read together, and links a watch to the
   * setting under the same value lock, so a write that stores after the read reaches the watch.
   */
  T SnapshotLinked(u64& generation, std::shared_ptr<detail::SettingWatch> watch);

  T default_;
  T staged_;
  Storage active_;
  /**
   * Unranked. It takes only the watches_mutex of the setting, which takes no lock and is never held
   * while a value lock is taken, so the pair has one order and no cycle. It calls no other code.
   */
  mutable std::mutex mutex_;
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
  friend class ConsoleCommand;
  friend class SettingsRegistry;
  struct State;
  std::unique_ptr<State> state_;
};

/**
 * Holds back the change calls for writes the calling thread makes while it lives. Each write
 * stores and returns its result immediately. When the outermost batch on the thread ends, each
 * function watching a changed setting is called once with the current values. A batch inside
 * another batch joins it. Other threads see the values immediately and their calls are not held
 * back.
 */
class REX_API SettingsBatch final : public NonCopyable {
 public:
  SettingsBatch();
  ~SettingsBatch();
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
inline void ApplyTrait(SettingConstraints& c, SettingMode mode) {
  c.mode = mode;
}
inline void ApplyTrait(SettingConstraints& c, NoSave) {
  c.no_save = true;
}
inline void ApplyTrait(SettingConstraints& c, DebugOnly) {
  c.debug_only = true;
}
inline void ApplyTrait(SettingConstraints& c, KeyChordTrait) {
  c.key_chord = true;
}

/** A watch over settings of fixed types, called with one value per setting. */
template <typename... T>
class SettingGroupWatch final : public SettingWatch {
 public:
  SettingGroupWatch(MoveOnlyFunction<void(const T&...)> fn, Setting<T>&... settings)
      : SettingWatch({&settings...}), members_(&settings...), fn_(std::move(fn)) {}

  /**
   * Links the watch to each setting under that setting's value lock, holding the call meanwhile,
   * without calling the function.
   *
   * @param self This watch.
   */
  void Start(const std::shared_ptr<SettingWatch>& self) {
    const Hold hold(*this);
    StartWith(self, std::index_sequence_for<T...>{});
  }

 private:
  template <size_t... I>
  void StartWith(const std::shared_ptr<SettingWatch>& self, std::index_sequence<I...>) {
    // A fold over the comma operator evaluates in order, so the settings link one after another.
    (LinkOne<I>(self), ...);
  }

  template <size_t I>
  void LinkOne(const std::shared_ptr<SettingWatch>& self) {
    u64 generation = 0;
    (void)std::get<I>(members_)->SnapshotLinked(generation, self);
    MarkSeen(I, generation);
  }

  void Call() override { CallWith(std::index_sequence_for<T...>{}); }

  template <size_t... I>
  void CallWith(std::index_sequence<I...>) {
    std::tuple<T...> values{Read<I>()...};
    std::apply(fn_, values);
  }

  template <size_t I>
  auto Read() {
    u64 generation = 0;
    auto value = std::get<I>(members_)->Snapshot(generation);
    MarkSeen(I, generation);
    return value;
  }

  std::tuple<Setting<T>*...> members_;
  MoveOnlyFunction<void(const T&...)> fn_;
};

/** A watch over a list of settings of one type, called with the values in list order. */
template <typename T>
class SettingListWatch final : public SettingWatch {
 public:
  SettingListWatch(MoveOnlyFunction<void(std::span<const T>)> fn,
                   std::span<Setting<T>* const> settings)
      : SettingWatch(std::vector<SettingBase*>(settings.begin(), settings.end())),
        members_(settings.begin(), settings.end()),
        fn_(std::move(fn)) {}

 private:
  void Call() override {
    std::vector<T> values;
    values.reserve(members_.size());
    for (size_t i = 0; i < members_.size(); ++i) {
      u64 generation = 0;
      values.push_back(members_[i]->Snapshot(generation));
      MarkSeen(i, generation);
    }
    fn_(std::span<const T>(values));
  }

  std::vector<Setting<T>*> members_;
  MoveOnlyFunction<void(std::span<const T>)> fn_;
};

}  // namespace detail

/**
 * Calls a function with the current values of several settings before Bind returns, and again
 * after any of them changes, as one watch. The rules of Setting::OnChange and of Setting::Bind
 * apply. Changes to several of them in one batch, or made by the change calls of one write while
 * the watch waits to be called, make one call.
 *
 * @code
 * auto guard = rex::Bind(
 *     [](const std::string& level, const bool& console) { Apply(level, console); },
 *     logger.level, logger.console);
 * @endcode
 *
 * @tparam T The value types.
 * @param fn Called with the current value of each setting, in the order given.
 * @param settings The settings, one or more.
 * @return The guard, as for Setting::OnChange.
 */
template <typename... T>
[[nodiscard]] ScopeGuard Bind(std::type_identity_t<MoveOnlyFunction<void(const T&...)>> fn,
                              Setting<T>&... settings) {
  static_assert(sizeof...(T) > 0, "Bind watches one or more settings");
  return detail::AddWatch(
      std::make_shared<detail::SettingGroupWatch<T...>>(std::move(fn), settings...));
}

/**
 * Calls a function with the current values of a list of settings before Bind returns, and again
 * after any of them changes, as one watch. The rules of the group Bind apply.
 *
 * @tparam T The value type.
 * @param fn Called with the current values, in list order.
 * @param settings The settings, one or more, each listed once.
 * @return The guard, as for Setting::OnChange.
 */
template <typename T>
[[nodiscard]] ScopeGuard Bind(std::type_identity_t<MoveOnlyFunction<void(std::span<const T>)>> fn,
                              std::span<Setting<T>* const> settings) {
  return detail::AddWatch(std::make_shared<detail::SettingListWatch<T>>(std::move(fn), settings));
}

template <typename T>
template <typename... Traits>
Setting<T>::Setting(SettingOwner* owner, std::string_view name, const T& default_value,
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
T Setting<T>::Snapshot(u64& generation) const {
  std::lock_guard lock(mutex_);
  generation = GetGeneration();
  if constexpr (kScalar) {
    return active_.load(std::memory_order_relaxed);
  } else {
    return active_;
  }
}

template <typename T>
T Setting<T>::SnapshotLinked(u64& generation, std::shared_ptr<detail::SettingWatch> watch) {
  std::lock_guard lock(mutex_);
  generation = GetGeneration();
  LinkWatch(std::move(watch));
  if constexpr (kScalar) {
    return active_.load(std::memory_order_relaxed);
  } else {
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
  auto watch = std::make_shared<detail::SettingGroupWatch<T>>(std::move(fn), *this);
  ScopeGuard guard = detail::SettingWatch::Guard(watch);
  watch->Start(watch);
  return guard;
}

template <typename T>
ScopeGuard Setting<T>::Bind(MoveOnlyFunction<void(const T&)> fn) {
  return rex::Bind<T>(std::move(fn), *this);
}

template <typename T>
Result<void> Setting<T>::Commit(SettingLayer layer, std::string_view text, CommitKind kind) {
  T parsed{};
  if (!detail::ParseSetting(text, parsed)) {
    return StoreFailed(text, kind);
  }
  bool published = false;
  {
    std::lock_guard lock(mutex_);
    if (auto admitted = Admit(layer, kind); !admitted) {
      return admitted;
    }
    const bool changed = detail::RenderSetting(staged_) != detail::RenderSetting(parsed);
    staged_ = std::move(parsed);
    published = FinishCommit(layer, kind, changed);
    if (published) {
      if constexpr (kScalar) {
        active_.store(staged_, std::memory_order_relaxed);
      } else {
        active_ = staged_;
      }
      AdvanceGeneration();
    }
  }
  if (published) {
    Notify();
  }
  return Ok();
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
void Setting<T>::WaitForCommit() const {
  std::lock_guard lock(mutex_);
}

}  // namespace rex

/**
 * Declares a setting inside a SettingOwner.
 *
 * @code
 * REX_SETTING(i32, count, 4, "How many", Range{1, 8}, kRequiresRestart);
 * @endcode
 *
 * @param name The member name. The full name is the owner's scope plus this.
 * @param default_value A path default may name other paths as {scope.name}.
 * @param ... Optional traits such as Range, Allowed, kRequiresRestart, kInitOnly and kLockedInGame.
 */
#define REX_SETTING(Type, name, default_value, help, ...)       \
  ::rex::Setting<Type> name {                                   \
    this, #name, default_value, help __VA_OPT__(, ) __VA_ARGS__ \
  }
