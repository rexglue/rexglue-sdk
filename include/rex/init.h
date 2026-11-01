/**
 * @file        init.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>

namespace rex {

struct Config {
  /** The module libraries to load, in order, such as rexhid. */
  std::vector<std::string> modules;
  /** Modules compiled into the program. They go before the libraries. */
  std::vector<IModule*> code_modules;
  /**
   * Runs after every listed module connects and before any module inits. Returns the plugin
   * libraries to load next, such as the drivers the settings select. Empty for none.
   *
   * @param factory Finds the public interfaces of the connected modules.
   * @param private_factory Finds the private interfaces, for the app only.
   * @return The plugin library names, or the reason the program cannot start.
   */
  std::function<Result<std::vector<std::string>>(IFactory& factory, IFactory& private_factory)>
      select_plugins;
};

/**
 * Loads and connects the listed modules in order, loads the plugins, then inits every module. A
 * module that requires an interface only a later module serves fails at Connect. A failure shuts
 * down and disconnects the modules in reverse. After Stop, Init runs the pending Shutdown first.
 *
 * @return Success, or the reason a module could not load, connect or init.
 */
[[nodiscard]] REX_API Result<void> Init(const Config& config);

/**
 * Shuts down and disconnects every module in reverse order and makes API::Get return null. The
 * libraries, settings and logger stay open until Shutdown. Does nothing when called again or before
 * Init.
 */
REX_API void Stop();

/** Runs Stop if needed, unloads the modules and ends the settings and logger sessions. */
REX_API void Shutdown();

}  // namespace rex
