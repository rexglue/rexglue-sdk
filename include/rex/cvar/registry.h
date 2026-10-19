/**
 * @file        cvar/registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/move_only_function.h>
#include <rex/module.h>

namespace rex::cvar {

class VarBase;
class CallbackHandle;

/** Where a cvar value came from, ascending precedence. */
enum class CvarSource : u8 {
  kDefault,     /**< The declared default. */
  kConfig,      /**< A TOML config file. */
  kEnvironment, /**< A REX_<SCOPE>_<LEAF> environment variable. */
  kCommandLine, /**< A command line option. */
  kRuntime,     /**< A set after Init through the registry. */
};

/**
 * Fires after a value changes, on the thread that changed it, with the registry mutex released.
 */
using ChangeCallback = rex::MoveOnlyFunction<void(std::string_view name, std::string_view value)>;

/**
 * The cvar registry. Core serves it. Every method takes the registry
 * mutex. Callbacks run after it is released.
 */
REX_INTERFACE(ICvarRegistry, "rex.cvar", 1)
 public:
  /**
   * Reserves scope for the caller and registers every var. Every name must be scope.leaf.
   * Fails with DuplicateScope, DuplicateName or BadName and registers nothing on failure.
   *
   * @param scope The scope to reserve.
   * @param vars The vars to register, each named scope.leaf.
   * @return Ok, or the error.
   */
  [[nodiscard]] virtual Result<void> Register(std::string_view scope,
                                              std::span<const VarBase* const> vars) = 0;
  /**
   * Adds vars to scopes that are reserved, for vars created at runtime such as keybinds.
   * A value Init received for a name in a scope reserved with an empty table waits here and is
   * applied when the name arrives, so a config file can hold keybinds. Fails with BadName when a
   * var's scope is not reserved, or with DuplicateName.
   *
   * @param vars The vars to add.
   * @return Ok, or the error.
   */
  [[nodiscard]] virtual Result<void> Add(std::span<const VarBase* const> vars) = 0;
  /**
   * Removes the vars. A scope reserved through Register with vars is released when its last var
   * leaves. A scope reserved with an empty table stays reserved.
   *
   * @param vars The vars to remove.
   */
  virtual void Unregister(std::span<const VarBase* const> vars) = 0;
  /**
   * Sets a cvar. Fails with NotFound, Invalid, InitOnly after finalize, or Locked.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @param value The value as text.
   * @return Ok, or the error.
   */
  [[nodiscard]] virtual Result<void> Set(std::string_view name, std::string_view value) = 0;
  /**
   * Renders the current value. Fails with NotFound.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @return The value as text, or the error.
   */
  [[nodiscard]] virtual Result<std::string> Get(std::string_view name) const = 0;
  /**
   * Changes the default and applies it when the current source is Default.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @param value The new default as text.
   * @return Ok, or the error.
   */
  [[nodiscard]] virtual Result<void> SetDefault(std::string_view name, std::string_view value) = 0;
  /**
   * Locks a cvar so every later Set fails with Locked. Values applied stay.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @return Ok, or NotFound.
   */
  [[nodiscard]] virtual Result<void> Lock(std::string_view name) = 0;
  /**
   * Restores the default, the source becomes Default, callbacks fire.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @return Ok, or NotFound.
   */
  [[nodiscard]] virtual Result<void> ResetToDefault(std::string_view name) = 0;
  /**
   * Looks up a var.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @return The var, or empty when the name is not registered. Valid until the var is
   *         unregistered. Only GetDesc(), Render() and GetDefaultText() are read on it.
   */
  [[nodiscard]] virtual std::optional<std::reference_wrapper<const VarBase>> Find(
      std::string_view name) const = 0;
  /**
   * Reports where a value came from.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @return Where the value came from. Default for an unknown name.
   */
  virtual CvarSource Source(std::string_view name) const = 0;
  /**
   * Lists registered vars.
   *
   * @param scope A scope to list, or empty for every scope.
   * @return Every registered var, or those of 1 scope, in registration order.
   */
  virtual std::vector<const VarBase*> Enumerate(std::string_view scope = {}) const = 0;
  /**
   * Lists reserved scopes.
   *
   * @return Every reserved scope, in reservation order.
   */
  virtual std::vector<std::string_view> GetScopes() const = 0;
  /**
   * Registers a change callback. Fails with NotFound.
   *
   * @param name Dotted cvar name such as gpu.vsync.
   * @param callback Called after each change.
   * @return The handle that owns the registration, or the error.
   */
  [[nodiscard]] virtual Result<CallbackHandle> OnChange(std::string_view name,
                                                        ChangeCallback callback) = 0;
  /**
   * Runs a command. Fails with NotFound or NotACommand.
   *
   * @param name Dotted command name such as console.echo.
   * @param args The argument text.
   * @return Ok, or the error.
   */
  [[nodiscard]] virtual Result<void> Invoke(std::string_view name, std::string_view args) = 0;
  /**
   * Lists the cvars waiting for a restart.
   *
   * @return RequiresRestart vars changed after finalize.
   */
  virtual std::vector<std::string> GetPendingRestart() const = 0;
  /**
   * Renders the non default values as TOML.
   *
   * @return Non default values as TOML, 1 [scope] table per scope, strings escaped.
   */
  virtual std::string Serialize() const = 0;
  /**
   * Reports whether Init finalized the registry.
   *
   * @return True after Init finalized the registry.
   */
  virtual bool IsFinalized() const = 0;

 protected:
  friend class CallbackHandle;
  /**
   * Removes a change callback registration by id.
   *
   * @param id The registration id.
   */
  virtual void RemoveCallback(u64 id) = 0;
};

/**
 * Owns a change callback registration. The destructor unregisters. Move only.
 */
class CallbackHandle : public MoveOnly {
 public:
  /**
   * Constructs an empty handle that owns nothing.
   */
  CallbackHandle() = default;
  /**
   * Constructs a handle that owns a registration.
   *
   * @param registry The registry that issued the handle.
   * @param id The registration id inside the registry.
   */
  CallbackHandle(ICvarRegistry* registry, u64 id) : registry_(registry), id_(id) {}
  /**
   * Takes over the registration of other, which becomes empty.
   *
   * @param other The handle to take over.
   */
  CallbackHandle(CallbackHandle&& other) noexcept
      : registry_(std::exchange(other.registry_, nullptr)), id_(std::exchange(other.id_, 0)) {}
  /**
   * Releases this registration and takes over the registration of other.
   *
   * @param other The handle to take over, which becomes empty.
   * @return This handle.
   */
  CallbackHandle& operator=(CallbackHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      registry_ = std::exchange(other.registry_, nullptr);
      id_ = std::exchange(other.id_, 0);
    }
    return *this;
  }
  /**
   * Unregisters.
   */
  ~CallbackHandle() { Reset(); }
  /**
   * Reports whether the handle owns a registration.
   *
   * @return True while the handle owns a registration.
   */
  explicit operator bool() const { return registry_ != nullptr; }
  /**
   * Unregisters now instead of at destruction.
   */
  void Reset() {
    if (registry_) {
      registry_->RemoveCallback(id_);
      registry_ = nullptr;
      id_ = 0;
    }
  }

 private:
  ICvarRegistry* registry_ = nullptr;
  u64 id_ = 0;
};

}  // namespace rex::cvar
