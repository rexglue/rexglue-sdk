/**
 * @file        api.h
 * @brief       SDK entry points, configuration and the Api handle
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rex/graphics/backend.h>
#include <rex/module.h>
#include <rex/result.h>

namespace rex {

namespace detail {
class ModuleLoader;
}

/**
 * Structural configuration for rex::Init.
 */
struct Config {
  /**
   * Module DLL base names to load, in load and Init order. Empty means no modules.
   */
  std::vector<std::string> modules;

  /**
   * Graphics backend slice, read by the graphics module at Connect.
   */
  graphics::BackendConfig graphics;

  /**
   * False keeps module DLLs mapped after Shutdown. Transitional. ReXApp sets it false because
   * the current shutdown path can still reach module code after Shutdown, and module cvars
   * are still static registrars. Removed with the lifecycle rework that makes shutdown complete.
   */
  bool unload_modules_at_shutdown = true;
};

/**
 * Handle to the initialized SDK. Owned by the loader, valid between Init and Shutdown.
 */
class Api {
 public:
  /**
   * Fetches an interface a loaded module serves.
   *
   * @return The interface, or null when no loaded module serves T::kInterfaceName at
   *         T::kInterfaceVersion.
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

 private:
  friend class detail::ModuleLoader;
  explicit Api(detail::ModuleLoader* loader) : loader_(loader) {}
  void* Get(const char* name, uint32_t version) const;
  detail::ModuleLoader* loader_;
};

/**
 * Loads, connects and initializes the modules named in the configuration.
 *
 * @param config The consumer's configuration. Copied and kept until Shutdown.
 * @return The Api handle, or an error naming the module and the reason.
 */
Result<Api*> Init(const Config& config);

/**
 * Shuts modules down in reverse order, disconnects them, unloads them. Cannot fail.
 */
void Shutdown();

/**
 * Reads the --modules argument into the configuration.
 *
 * @param config Receives the module names, appended in order.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return The arguments that were not consumed, in order, without argv[0].
 */
std::vector<std::string> ParseCommandLine(Config& config, int argc, char** argv);

}  // namespace rex
