/**
 * @file        api.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/apu.h>
#include <rex/cli_action.h>
#include <rex/cli_arg_list.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/gpu.h>
#include <rex/hid.h>
#include <rex/logging/config.h>
#include <rex/module.h>
#include <rex/paths.h>
#include <rex/result.h>
#include <rex/runtime/config.h>
#include <rex/types.h>
#include <rex/ui.h>

namespace rex {

namespace detail {
class ModuleLoader;
}

/**
 * Structural configuration for rex::Init. Filled by the consumer, copied by Init, read through
 * API::GetConfig().
 */
struct Config {
  Config() = default;
  /** Copies the configuration. */
  Config(const Config&) = default;
  /** Copy assigns the configuration. */
  Config& operator=(const Config&) = default;
  /** Moves the configuration. */
  Config(Config&&) noexcept(false) = default;
  /** Move assigns the configuration. */
  Config& operator=(Config&&) noexcept(false) = default;
  ~Config() = default;

  /** Module DLL base names to load, in load and Init order. Empty means no modules. */
  std::vector<std::string> modules;
  /** Host paths. Empty fields take the defaults documented on PathsConfig. */
  PathsConfig paths;
  /** Logger config, read when the modules connect. */
  LogConfig log;
  /** Consumer cvar tables, values arriving before Init, and locks. */
  CvarConfig cvars;
  /** Filesystem module config. */
  filesystem::FilesystemConfig filesystem;
  /** Runtime module config. */
  runtime::RuntimeConfig runtime;
  /** APU config, read by the runtime. */
  apu::APUConfig apu;
  /** HID config, read by the loaded hid module at Connect. */
  hid::HIDConfig hid;
  /** UI module config. */
  ui::UIConfig ui;
  /** GPU backend config, read by the GPU backend module at Connect. */
  gpu::GPUConfig gpu;
  /** App::Init starts the Tracy profiler when the build has it. */
  bool profiler = false;
  /**
   * Name of a lock across processes that App::Init takes before any module loads. Init fails
   * in a second process started with the same name. Empty means no lock.
   */
  std::string single_instance;
};

/**
 * Handle to the initialized SDK. Owned by the loader, valid between Init and Shutdown.
 */
class REX_API API {
 public:
  /**
   * Fetches an interface a loaded module or core serves.
   *
   * @tparam T An interface type with kInterfaceName and kInterfaceVersion.
   * @return The interface, or null when nothing serves T::kInterfaceName at T::kInterfaceVersion.
   */
  template <typename T>
  T* Get() const {
    return static_cast<T*>(Get(T::kInterfaceName, T::kInterfaceVersion));
  }

  /**
   * Reports whether a module was loaded and initialized.
   *
   * @param module_name Module DLL base name such as rexgpu-xenos.
   * @return True when the module was loaded and initialized.
   */
  bool IsLoaded(std::string_view module_name) const;

  /**
   * Returns the configuration Init copied.
   *
   * @return The copied configuration. Valid until Shutdown.
   */
  const Config& GetConfig() const;

  /**
   * Replaces the paths of the copied configuration.
   *
   * @param paths The new paths.
   */
  void UpdatePaths(const PathsConfig& paths);

 private:
  friend class detail::ModuleLoader;
  explicit API(detail::ModuleLoader* loader) : loader_(loader) {}
  void* Get(const char* name, u32 version) const;
  detail::ModuleLoader* loader_;
};

/**
 * Loads, connects and initializes the modules named in the configuration, registers every cvar
 * table, applies the values in config.cvars and finalizes the registry.
 *
 * @param config The consumer's configuration. Copied and kept until Shutdown.
 * @return The API handle, or an error naming the module, cvar or field and the reason.
 */
[[nodiscard]] REX_API Result<API*> Init(const Config& config);

/**
 * Shuts modules down in reverse order, disconnects them, unregisters every cvar table, unloads
 * the modules. Cannot fail.
 */
REX_API void Shutdown();

/**
 * Applies command line options to a configuration. Options with a structural field (such as
 * --game_data_root, --modules, --backend) fill that field. The first --modules replaces the module
 * list of the configuration and later ones append to it. --scope.name=value and --scope.name set a
 * cvar, --no-scope.name sets it false and is accepted only for a bool cvar.
 *
 * An option named like a declared action goes to hits and never to the cvars. An action is
 * matched before a structural field and before a cvar, and --no-name is not an action.
 *
 * @param config The configuration to fill.
 * @param args The parsed command line.
 * @param actions The actions the app declares.
 * @param hits Receives each declared action found, in command line order. May be null.
 * @return The options it did not recognize, as --name or --name=value, in order.
 */
REX_API std::vector<std::string> ParseCommandLine(Config& config, const CLIArgList& args,
                                                  std::span<const CLIAction> actions = {},
                                                  std::vector<CLIActionHit>* hits = nullptr);

/**
 * Checks the actions an app declares.
 *
 * @param actions The declared actions.
 * @return Ok, or a Config error that names a name used twice, a name that is not a valid action
 *         name, or a name shared with a structural command line field.
 */
[[nodiscard]] REX_API Result<void> ValidateCLIActions(std::span<const CLIAction> actions);

/**
 * Parses a TOML file into config.cvars.values. A [scope] table with leaf keys and a flat
 * scope.leaf key both name the cvar scope.leaf. A missing file is Ok.
 *
 * @param config The configuration to fill.
 * @param path The TOML file.
 * @return Ok, or a Config error with the parse message and the path.
 */
[[nodiscard]] REX_API Result<void> LoadConfigFile(Config& config,
                                                  const std::filesystem::path& path);

/**
 * Writes the registry's non default values as TOML, 1 [scope] table per scope.
 *
 * @param registry The registry to read.
 * @param path The TOML file to write.
 * @return Ok, or a Config error with the OS message and the path.
 */
[[nodiscard]] REX_API Result<void> SaveConfigFile(const cvar::ICvarRegistry& registry,
                                                  const std::filesystem::path& path);

}  // namespace rex
