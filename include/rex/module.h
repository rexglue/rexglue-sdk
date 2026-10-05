/**
 * @file        module.h
 * @brief       Module lifecycle interface and the module DLL export contract
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {

struct Config;

/**
 * Result of an interface request at the module boundary.
 */
enum class InterfaceStatus : i32 {  // NOLINT(performance-enum-size): the C export reports it as i32
  Ok = 0,                           /**< The interface is served. */
  NotFound = 1,                     /**< No interface of that name. */
  VersionMismatch = 2,              /**< The name is served at a different version. */
};

/**
 * Serves interfaces by name and version. The loader implements it and passes it to Connect.
 */
class IInterfaceFactory {
 public:
  virtual ~IInterfaceFactory() = default;

  /**
   * Fetches an interface.
   *
   * @param name Interface name, the kInterfaceName of the requested interface class.
   * @param version Version the caller was compiled against.
   * @return The interface, or a Module error whose code is the InterfaceStatus.
   */
  [[nodiscard]] virtual Result<void*> Get(std::string_view name, u32 version) = 0;

  /**
   * Typed fetch by T::kInterfaceName and T::kInterfaceVersion.
   *
   * @tparam T The interface class, which declares kInterfaceName and kInterfaceVersion.
   * @return The interface, or a Module error whose code is the InterfaceStatus.
   */
  template <typename T>
  [[nodiscard]] Result<T*> Get() {
    auto found = Get(T::kInterfaceName, T::kInterfaceVersion);
    if (!found) {
      return std::unexpected(std::move(found).error());
    }
    return static_cast<T*>(*found);
  }
};

/**
 * Lifecycle of a module. Every module DLL serves it.
 */
class IModule {
 public:
  REX_INTERFACE(IModule, "rex.module", 1);

  virtual ~IModule() = default;

  /**
   * Returns the module's DLL base name.
   *
   * @return The DLL base name, for example "rex-gpu-xenos".
   */
  virtual const char* Name() const = 0;

  /**
   * Returns the interfaces this module needs from modules listed before it.
   *
   * @return The required interfaces, each as "name@version".
   */
  virtual std::span<const char* const> RequiredInterfaces() const = 0;

  /**
   * Acquires pointers and registers descriptors. Allocates nothing that needs another
   * module initialized.
   *
   * @param factory Serves every loaded module's interfaces.
   * @param config The consumer's configuration. The module keeps a pointer to its module config.
   * @return Ok, or an error that aborts Init before any module's Init runs.
   */
  [[nodiscard]] virtual Result<void> Connect(IInterfaceFactory* factory, const Config& config) = 0;

  /**
   * Releases what Connect acquired. Cannot fail.
   */
  virtual void Disconnect() = 0;

  /**
   * Builds the module's state from its module config.
   *
   * @return Ok, or an error that unwinds every module initialized before it.
   */
  [[nodiscard]] virtual Result<void> Init() = 0;

  /**
   * Tears down what Init built. Cannot fail.
   */
  virtual void Shutdown() = 0;
};

/**
 * 1 served interface in a module's table.
 */
struct InterfaceEntry {
  const char* name; /**< Dotted interface name. */
  u32 version;      /**< Served version. */
  void* (*get)();   /**< Returns the interface instance. */
};

/**
 * Name of the C export that serves interfaces.
 */
inline constexpr const char* kCreateInterfaceSymbol = "rex_create_interface";

/**
 * Name of the C export that returns the served interface table.
 */
inline constexpr const char* kEnumerateInterfacesSymbol = "rex_enumerate_interfaces";

/**
 * Signature of rex_create_interface.
 */
using CreateInterfaceFn = void* (*)(const char* name, u32 version, i32* out_status);

/**
 * Signature of rex_enumerate_interfaces.
 */
using EnumerateInterfacesFn = void (*)(const InterfaceEntry** out_entries, u32* out_count);

}  // namespace rex
