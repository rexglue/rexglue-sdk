/**
 * @file        cvar/registry.h
 * @brief       The cvar registry interface, served by core
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::cvar {

class IVar;
class CallbackHandle;

/** Where a cvar value came from. Defined in rex/cvar.h. */
enum class CvarSource : u8;

/**
 * Fires after a value changes, on the thread that changed it, with the registry mutex released.
 */
using ChangeCallback = std::move_only_function<void(std::string_view name, std::string_view value)>;

/**
 * The cvar registry. Core serves it. Every method takes the registry
 * mutex. Callbacks run after it is released.
 */
class ICvarRegistry {
 public:
  REX_INTERFACE(ICvarRegistry, "rex.cvar", 1);
  virtual ~ICvarRegistry() = default;

  /**
   * Reserves scope for the caller and registers every var. Every name must be scope.leaf.
   * Fails with DuplicateScope, DuplicateName or BadName and registers nothing on failure.
   *
   * @param scope The scope to reserve.
   * @param vars The vars to register, each named scope.leaf.
   * @return Ok, or the error.
   */
  virtual Result<void> Register(std::string_view scope, std::span<const IVar* const> vars) = 0;
  /**
   * Adds vars to scopes that are already reserved, for vars created at runtime such as keybinds.
   * A value Init received for a name in a scope reserved with an empty table waits here and is
   * applied when the name arrives, so a config file can hold keybinds. Fails with BadName when a
   * var's scope is not reserved, or with DuplicateName.
   *
   * @param vars The vars to add.
   * @return Ok, or the error.
   */
  virtual Result<void> Add(std::span<const IVar* const> vars) = 0;
  /**
   * Removes the vars. A scope reserved through Register with vars is released when its last var
   * leaves. A scope reserved with an empty table stays until Reset.
   *
   * @param vars The vars to remove.
   */
  virtual void Unregister(std::span<const IVar* const> vars) = 0;
  /**
   * Sets with source Runtime, validated. Fails with NotFound, Invalid, InitOnly after
   * finalize, or Locked.
   *
   * @param name The cvar name.
   * @param value The value as text.
   * @return Ok, or the error.
   */
  virtual Result<void> Set(std::string_view name, std::string_view value) = 0;
  /**
   * Renders the current value. Fails with NotFound.
   *
   * @param name The cvar name.
   * @return The value as text, or the error.
   */
  virtual Result<std::string> Get(std::string_view name) const = 0;
  /**
   * Changes the default and applies it when the current source is Default.
   *
   * @param name The cvar name.
   * @param value The new default as text.
   * @return Ok, or the error.
   */
  virtual Result<void> SetDefault(std::string_view name, std::string_view value) = 0;
  /**
   * Set fails with Locked from now on. Values already applied stay.
   *
   * @param name The cvar name.
   * @return Ok, or NotFound.
   */
  virtual Result<void> Lock(std::string_view name) = 0;
  /**
   * Restores the default, the source becomes Default, callbacks fire.
   *
   * @param name The cvar name.
   * @return Ok, or NotFound.
   */
  virtual Result<void> ResetToDefault(std::string_view name) = 0;
  /**
   * Looks up a var.
   *
   * @param name The cvar name.
   * @return The var, or null. Valid until the var is unregistered. Read desc(), Render() and
   *         DefaultText() on it, never Parse or Invoke, those go through Set and Invoke here.
   */
  virtual const IVar* Find(std::string_view name) const = 0;
  /**
   * Reports where a value came from.
   *
   * @param name The cvar name.
   * @return Where the value came from. Default for an unknown name.
   */
  virtual CvarSource Source(std::string_view name) const = 0;
  /**
   * Lists registered vars.
   *
   * @param scope A scope to list, or empty for every scope.
   * @return Every registered var, or those of 1 scope, in registration order.
   */
  virtual std::vector<const IVar*> Enumerate(std::string_view scope = {}) const = 0;
  /**
   * @return Every reserved scope, in reservation order.
   */
  virtual std::vector<std::string_view> Scopes() const = 0;
  /**
   * Registers a change callback. Fails with NotFound.
   *
   * @param name The cvar name.
   * @param callback Called after each change.
   * @return The handle that owns the registration, or the error.
   */
  virtual Result<CallbackHandle> OnChange(std::string_view name, ChangeCallback callback) = 0;
  /**
   * Runs a command. Fails with NotFound or NotACommand.
   *
   * @param name The command name.
   * @param args The argument text.
   * @return Ok, or the error.
   */
  virtual Result<void> Invoke(std::string_view name, std::string_view args) = 0;
  /**
   * @return RequiresRestart vars changed with source Runtime after finalize.
   */
  virtual std::vector<std::string> PendingRestart() const = 0;
  /**
   * @return Non default values as TOML, 1 [scope] table per scope, strings escaped.
   */
  virtual std::string Serialize() const = 0;
  /**
   * @return True after Init finalized the registry.
   */
  virtual bool IsFinalized() const = 0;

 protected:
  friend class CallbackHandle;
  /**
   * Called by CallbackHandle. Removes 1 registration by id.
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
   * An empty handle that owns nothing.
   */
  CallbackHandle() = default;
  /**
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
