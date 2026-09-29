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

#include <rex/cvar.h>
#include <rex/filesystem/config.h>
#include <rex/graphics/backend.h>
#include <rex/input.h>
#include <rex/module.h>
#include <rex/paths.h>
#include <rex/result.h>
#include <rex/runtime/config.h>
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
   * False keeps module DLLs mapped after Shutdown. Transitional. ReXApp sets it false because
   * the current shutdown path can still reach module code after Shutdown. Deleted in sub-project 6.
   */
  bool unload_modules_at_shutdown = true;
};

/**
 * Handle to the initialized SDK. Owned by the loader, valid between Init and Shutdown.
 */
class API {
 public:
  /**
   * Fetches an interface a loaded module or core serves.
   *
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
   */
  void UpdatePaths(const PathsConfig& paths);

 private:
  friend class detail::ModuleLoader;
  explicit API(detail::ModuleLoader* loader) : loader_(loader) {}
  void* Get(const char* name, uint32_t version) const;
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
 * Reads --modules, the structural fields with a command line form (--game_data_root,
 * --user_data_root, --update_data_root, --cache_root, --metadata_root, --config_file,
 * --backend) and every --scope.leaf cvar form into config. Cvar forms are --name=value,
 * --name value, --name (true) and --no-name (false).
 *
 * @return The arguments that were not consumed, in order. Positionals and unknown -- arguments.
 */
std::vector<std::string> ParseCommandLine(Config& config, int argc, char** argv);

/**
 * Parses a TOML file into config.cvars.values with source Config. A [scope] table with leaf keys
 * and a flat scope.leaf key both name the cvar scope.leaf. A missing file is Ok.
 *
 * @return Ok, or a Config error with the parse message and the path.
 */
Status LoadConfigFile(Config& config, const std::filesystem::path& path);

/**
 * Writes the registry's non default values as TOML, 1 [scope] table per scope.
 *
 * @return Ok, or a Config error with the OS message and the path.
 */
Status SaveConfigFile(const cvar::ICvarRegistry& registry, const std::filesystem::path& path);

}  // namespace rex
