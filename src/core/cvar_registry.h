/**
 * @file        core/cvar_registry.h
 * @brief       The cvar registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/cvar.h>
#include <rex/cvar/config.h>

namespace rex::detail {

/**
 * ICvarRegistry with the Init side entry points the loader uses.
 */
class REX_API CvarRegistryImpl final : public cvar::ICvarRegistry {
 public:
  CvarRegistryImpl() = default;
  ~CvarRegistryImpl() override;

  [[nodiscard]] Result<void> Register(std::string_view scope,
                                      std::span<const cvar::VarBase* const> vars) override;
  [[nodiscard]] Result<void> Add(std::span<const cvar::VarBase* const> vars) override;
  void Unregister(std::span<const cvar::VarBase* const> vars) override;
  [[nodiscard]] Result<void> Set(std::string_view name, std::string_view value) override;
  [[nodiscard]] Result<std::string> Get(std::string_view name) const override;
  [[nodiscard]] Result<void> SetDefault(std::string_view name, std::string_view value) override;
  [[nodiscard]] Result<void> Lock(std::string_view name) override;
  [[nodiscard]] Result<void> ResetToDefault(std::string_view name) override;
  [[nodiscard]] std::optional<std::reference_wrapper<const cvar::VarBase>> Find(
      std::string_view name) const override;
  cvar::CvarSource Source(std::string_view name) const override;
  std::vector<const cvar::VarBase*> Enumerate(std::string_view scope = {}) const override;
  std::vector<std::string_view> GetScopes() const override;
  [[nodiscard]] Result<cvar::CallbackHandle> OnChange(std::string_view name,
                                                      cvar::ChangeCallback callback) override;
  [[nodiscard]] Result<void> Invoke(std::string_view name, std::string_view args) override;
  std::vector<std::string> GetPendingRestart() const override;
  std::string Serialize() const override;
  bool IsFinalized() const override;

  /**
   * Applies overrides in ascending source order to the names that are registered.
   *
   * An override whose scope is reserved with an empty table is kept and applied when Add registers
   * its name.
   *
   * @param overrides The overrides to apply.
   * @return The overrides whose names are not registered. A rejected value is an error naming the
   *         cvar, the value, the source and the reason.
   */
  [[nodiscard]] Result<std::vector<CvarOverride>> ApplyOverrides(
      std::span<const CvarOverride> overrides);
  /**
   * Locks every name.
   *
   * @param names The names to lock.
   * @return Ok, or an error when a name is unknown.
   */
  [[nodiscard]] Result<void> ApplyLocks(std::span<const std::string> names);
  /**
   * Marks the registry finalized. InitOnly is enforced and RequiresRestart marks pending from
   * here.
   */
  void Finalize();
  /**
   * Unregisters everything, clears callbacks, locks and the finalized flag. Values stay in their
   * cells.
   */
  void Reset();
  /**
   * Returns the names of every registered var.
   *
   * @return The names.
   */
  std::vector<std::string> GetRegisteredNames() const;

 private:
  struct Entry {
    const cvar::VarBase* var = nullptr;
    std::string default_override;
    bool has_default_override = false;
    cvar::CvarSource source = cvar::CvarSource::kDefault;
    bool locked = false;
  };
  struct Callback {
    u64 id;
    std::string name;
    cvar::ChangeCallback fn;
  };
  using Fired = std::vector<std::pair<std::string, std::string>>;

  void RemoveCallback(u64 id) override;
  Entry* FindEntry(std::string_view name);
  const Entry* FindEntry(std::string_view name) const;
  [[nodiscard]] Result<void> Validate(const Entry& entry, std::string_view value,
                                      cvar::CvarSource source) const;
  [[nodiscard]] Result<void> Apply(Entry& entry, std::string_view value, cvar::CvarSource source,
                                   Fired& fired);
  void Fire(Fired& fired);
  void ApplyPending(Entry& entry, Fired& fired);
  std::string_view DefaultOf(const Entry& entry) const;

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::unordered_map<std::string_view, size_t> index_;
  std::vector<std::string> scopes_;
  std::vector<std::string> pinned_scopes_;
  std::vector<CvarOverride> pending_;
  std::vector<std::shared_ptr<Callback>> callbacks_;
  std::vector<std::string> pending_restart_;
  u64 next_callback_id_ = 1;
  bool finalized_ = false;
};

/**
 * Creates a registry.
 *
 * @return The registry.
 */
REX_API std::unique_ptr<CvarRegistryImpl> CreateCvarRegistry();

}  // namespace rex::detail
