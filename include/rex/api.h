/**
 * @file        api.h
 * @brief       SDK entry points, configuration and the API handle
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
#include <string>
#include <string_view>
#include <vector>

#include <rex/cli_arg_list.h>
#include <rex/cvar.h>
#include <rex/filesystem/config.h>
#include <rex/graphics/backend.h>
#include <rex/input.h>
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
 * API::config().
 */
struct Config {
  /** Module DLL base names to load, in load and Init order. Empty means no modules. */
  std::vector<std::string> modules;
  /** Host paths. Empty fields take the defaults documented on PathsConfig. */
  PathsConfig paths;
  /** Consumer cvar tables, values arriving before Init, and locks. */
  CvarConfig cvars;
  /** Filesystem module slice. */
  filesystem::FilesystemConfig filesystem;
  /** Runtime module slice. */
  runtime::RuntimeConfig runtime;
  /** Input module slice. */
  input::InputConfig input;
  /** UI module slice. */
  ui::UIConfig ui;
  /** Graphics backend slice, read by the graphics module at Connect. */
  graphics::BackendConfig graphics;
  /**
   * Name of a lock across processes that App::Init takes before any module loads. Init fails
   * in a second process started with the same name. Empty means no lock.
   */
  std::string single_instance;
};

/**
 * Handle to the initialized SDK. Owned by the loader, valid between Init and Shutdown.
 */
class API {
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
   * @param module_name Module DLL base name.
   * @return True when the module was loaded and initialized.
   */
  bool IsLoaded(std::string_view module_name) const;

  /**
   * @return The configuration Init copied. Valid until Shutdown.
   */
  const Config& config() const;

  /**
   * Replaces the paths slice of the copied configuration. For hosts that resolve the game data
   * root after Init, before the Runtime is constructed from config().
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
Result<API*> Init(const Config& config);

/**
 * Shuts modules down in reverse order, disconnects them, unregisters every cvar table, unloads
 * the modules. Cannot fail.
 */
void Shutdown();

/**
 * Applies command line options to a configuration. Options with a structural field (such as
 * --game_data_root, --modules, --backend) fill that field. --scope.name=value and --scope.name
 * set a cvar, --no-scope.name sets it false.
 *
 * @param config The configuration to fill.
 * @param args The parsed command line.
 * @return The options it did not recognize, as --name or --name=value, in order.
 */
std::vector<std::string> ParseCommandLine(Config& config, const CLIArgList& args);

/**
 * Parses a TOML file into config.cvars.values with source Config. A [scope] table with leaf keys
 * and a flat scope.leaf key both name the cvar scope.leaf. A missing file is Ok.
 *
 * @param config The configuration to fill.
 * @param path The TOML file.
 * @return Ok, or a Config error with the parse message and the path.
 */
Result<void> LoadConfigFile(Config& config, const std::filesystem::path& path);

/**
 * Writes the registry's non default values as TOML, 1 [scope] table per scope.
 *
 * @param registry The registry to read.
 * @param path The TOML file to write.
 * @return Ok, or a Config error with the OS message and the path.
 */
Result<void> SaveConfigFile(const cvar::ICvarRegistry& registry, const std::filesystem::path& path);

}  // namespace rex
