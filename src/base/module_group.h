/**
 * @file        base/module_group.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {

/**
 * Owns the modules of one app and acts as their factory. Connect and Init run in list order,
 * Shutdown and Disconnect in reverse, and libraries unload in reverse when the group is destroyed.
 */
class REX_API ModuleGroup final : public IInterfaceFactory {
 public:
  ModuleGroup();

  ~ModuleGroup() override;

  /**
   * Serves a host interface, found before those of the modules.
   *
   * @param iface The interface, which must outlive the group.
   * @param type The name and version it is served under.
   */
  void Serve(IInterface& iface, InterfaceType type);

  /**
   * Returns the factory that also finds private interfaces. Only the app and the kernel take it.
   */
  IInterfaceFactory& GetPrivateFactory();

  /**
   * Adds a module compiled into the program. A module added after Connect connects at once, and
   * inits too when the group is inited.
   *
   * @param module The module, which must outlive the group.
   * @return Success, or why the module could not connect or init.
   */
  [[nodiscard]] Result<void> Add(IModule& module);

  /**
   * Adds modules compiled into the program as one batch. When the group is connected, the batch
   * connects in list order and then inits in list order. At Connect a module finds only the host
   * interfaces and the modules before it, at Init every connected module. A failure shuts down and
   * disconnects the batch and everything loaded since, in reverse, and removes them.
   *
   * @param modules The modules, which must outlive the group.
   * @return Success, or why a module could not connect or init.
   */
  [[nodiscard]] Result<void> Add(std::span<IModule* const> modules);

  /**
   * Loads one module by name from the executable folder, then from the install library folder. A
   * loaded module is not loaded again. The build the library reports is logged, one line for
   * each module, and not compared: a module loads on the interface names and versions it serves
   * and requires.
   *
   * @param module_name The module name without postfix or extension, such as rexapu-sdl3.
   * @return Success, or an error naming the module when it is missing, exports no module
   *         symbols, serves no IModule or another version of it, or fails to connect or init. An
   *         error ends with a line that names the build of the host and of each loaded library
   *         when they are not all the same.
   */
  [[nodiscard]] Result<void> Load(std::string_view module_name);

  /**
   * Sets a function called after each module inits and before each module shuts down.
   *
   * @param observer Called with the module and true after its Init, false before its Shutdown.
   */
  void Observe(std::function<void(IModule&, bool inited)> observer);

  /** Returns the number of modules in the list. It is the mark for ShutdownAfter. */
  [[nodiscard]] size_t Mark() const;

  /** Shuts down every inited module added after a mark, last added first. */
  void ShutdownAfter(size_t mark);

  /**
   * Connects every unconnected module in list order. A required interface that the host and the
   * earlier modules do not serve fails the module. A failure disconnects the earlier modules in
   * reverse.
   *
   * @return Success, or the error of the first module that failed.
   */
  [[nodiscard]] Result<void> Connect();

  /**
   * Inits every uninited module in list order. A failure shuts down the earlier modules in reverse
   * and disconnects all of them.
   *
   * @return Success, or the error of the first module that failed.
   */
  [[nodiscard]] Result<void> Init();

  /** Shuts down every inited module, last added first. */
  void Shutdown();

  /** Disconnects every connected module, last added first. */
  void Disconnect();

  /**
   * The host interfaces come first, then each module in list order.
   *
   * @param version The version, matched exactly.
   * @return The interface, or null when nothing serves that name and version.
   */
  IInterface* Find(std::string_view name, u32 version) override;

  /**
   * Finds the interface of every module that serves it.
   *
   * @param version The version, matched exactly.
   * @return The interfaces in list order.
   */
  std::vector<IInterface*> FindAll(std::string_view name, u32 version) override;

  using IInterfaceFactory::FindAll;

 private:
  struct State;

  std::unique_ptr<State> state_;
};

}  // namespace rex
