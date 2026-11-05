/**
 * @file        settings.h
 * @brief       The registry of every registered setting
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/setting.h>
#include <rex/utility.h>

namespace rex {

class ProcessSettingsControl;

/** A name with a value from a source and no registered setting. */
struct UnknownSetting {
  std::string name;   /**< The full name the source used, after the alias table. */
  SettingLayer layer; /**< The highest layer that holds a value for it. */
};

/** The registry of every registered owner's settings. Values for a later owner wait here. */
REX_DECLARE_INTERFACE(ISettings, 1)
/**
 * Registers an owner and applies values waiting for its settings.
 *
 * @return Ok, or an error naming both owners when a full name is already registered.
 */
  [[nodiscard]] virtual Result<void> Register(SettingOwner& owner) = 0;

  /**
   * Removes an owner. Its current values wait here for the next owner of the same names.
   */
  virtual void Unregister(SettingOwner& owner) = 0;

  /**
   * Finds a setting by full name.
   *
   * @return The setting, or kNotFound.
   */
  [[nodiscard]] virtual Result<SettingBase*> Find(std::string_view name) const = 0;

  /**
   * Lists registered settings, sorted by name.
   *
   * @param scope Only settings under this scope, every setting when empty.
   */
  virtual std::vector<SettingBase*> Enumerate(std::string_view scope = {}) const = 0;

  /**
   * Lists registered commands, sorted by name.
   *
   * @param scope Only commands under this scope, every command when empty.
   */
  virtual std::vector<Command*> EnumerateCommands(std::string_view scope = {}) const = 0;

  /**
   * Sets a setting from text at the runtime layer. An old name applies to its new setting.
   *
   * @return Ok, kNotFound, or the setting's refusal.
   */
  [[nodiscard]] virtual Result<void> SetText(std::string_view name, std::string_view text) = 0;

  /**
   * Sets a setting from text at a layer. A name nothing registered yet waits and applies when its
   * owner registers. A value a higher layer holds is kept, not an error.
   *
   * @param name The full name, or an old name from the alias table.
   * @return Ok, or the setting's refusal.
   */
  [[nodiscard]] virtual Result<void> SetText(SettingLayer layer, std::string_view name,
                                             std::string_view text) = 0;

  /**
   * Maps an old name to the setting that replaced it. Each use of an old name adds one notice.
   *
   * @param new_name The new full name, empty when the old name was removed and its values are
   * ignored.
   */
  virtual void AddAlias(std::string_view old_name, std::string_view new_name) = 0;

  /**
   * Reports whether a name is an old name in the alias table, removed names included.
   *
   * @param name The name to look up.
   * @return True when SetText resolves the name through an alias.
   */
  [[nodiscard]] virtual bool IsAlias(std::string_view name) const = 0;

  /**
   * Returns a setting to its default. The result is Ok, kNotFound, or an error when it is frozen.
   */
  [[nodiscard]] virtual Result<void> Reset(std::string_view name) = 0;

  /**
   * Runs a command.
   *
   * @return What the command returned, or kNotFound.
   */
  [[nodiscard]] virtual Result<void> Run(std::string_view name, std::string_view args) = 0;

  /** Lists the kRestart settings whose change waits for the next start. */
  virtual std::vector<std::string> GetRestartPending() const = 0;

  /**
   * Writes every saved setting that differs from its default to a TOML file.
   *
   * @return Ok, or an error when the file cannot be written.
   */
  [[nodiscard]] virtual Result<void> Save(const std::filesystem::path& file) const = 0;

  /**
   * Reads a TOML file into the runtime layer. Old names apply through the alias table.
   *
   * @param layer The layer the values apply at.
   * @return Ok, or an error naming the file and the first refused value.
   */
  [[nodiscard]] virtual Result<void> Load(const std::filesystem::path& file,
                                          SettingLayer layer = SettingLayer::kRuntime) = 0;
};

/** The process settings registry, reached through GetSettings() and, for modules, ISettings. */
class REX_API Settings final : public ISettings {
 public:
  [[nodiscard]] Result<void> Register(SettingOwner& owner) override;

  void Unregister(SettingOwner& owner) override;

  [[nodiscard]] Result<SettingBase*> Find(std::string_view name) const override;

  std::vector<SettingBase*> Enumerate(std::string_view scope = {}) const override;

  std::vector<Command*> EnumerateCommands(std::string_view scope = {}) const override;

  [[nodiscard]] Result<void> SetText(std::string_view name, std::string_view text) override;

  [[nodiscard]] Result<void> SetText(SettingLayer layer, std::string_view name,
                                     std::string_view text) override;

  void AddAlias(std::string_view old_name, std::string_view new_name) override;

  [[nodiscard]] bool IsAlias(std::string_view name) const override;

  [[nodiscard]] Result<void> Reset(std::string_view name) override;

  [[nodiscard]] Result<void> Run(std::string_view name, std::string_view args) override;

  std::vector<std::string> GetRestartPending() const override;

  [[nodiscard]] Result<void> Save(const std::filesystem::path& file) const override;

  [[nodiscard]] Result<void> Load(const std::filesystem::path& file,
                                  SettingLayer layer = SettingLayer::kRuntime) override;

 private:
  friend REX_API Settings& GetSettings();
  friend class ProcessSettingsControl;

  /** Returns every setting to its default, unlocks it and forgets waiting values. */
  void EndSession();

  /** Applies the REX_<SCOPE>_<NAME> environment variables at the environment layer. */
  void ApplyEnvironment();

  /**
   * Returns the messages the registry produced, such as an old name used, and clears them, oldest
   * first.
   */
  std::vector<std::string> TakeNotices();

  /**
   * Returns the names a source set that no owner has registered, each once, sorted, with the
   * highest layer of each.
   */
  std::vector<UnknownSetting> TakeUnknown();

  Settings();
  ~Settings();
  friend class SettingBase;
  friend class Command;
  void Detach(SettingBase& setting);
  void Detach(Command& command);
  bool ApplyOldLogFile(SettingLayer layer, std::string_view name, std::string_view text,
                       Result<void>& result);

  struct State;
  std::unique_ptr<State> state_;
};

REX_API Settings& GetSettings();

#define REX_SETTINGS() ::rex::GetSettings()

}  // namespace rex
