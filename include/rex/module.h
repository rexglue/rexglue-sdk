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

#include <cstdint>
#include <span>

#include <rex/macros.h>
#include <rex/result.h>

namespace rex {

struct Config;

/**
 * Result of an interface request at the module boundary.
 */
enum class InterfaceStatus : int32_t {
  Ok = 0,              /**< The interface is served. */
  NotFound = 1,        /**< No interface of that name. */
  VersionMismatch = 2, /**< The name is served at a different version. */
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
   * @param name Dotted interface name, for example "rex.graphics.backend".
   * @param version Version the caller was compiled against.
   * @param out_status Receives the status. May be null.
   * @return The interface, or null when the status is not Ok.
   */
  virtual void* Get(const char* name, uint32_t version, InterfaceStatus* out_status) = 0;

  /**
   * Typed fetch by T::kInterfaceName and T::kInterfaceVersion.
   *
   * @return The interface, or null when it is not served at that version.
   */
  template <typename T>
  T* Get() {
    return static_cast<T*>(Get(T::kInterfaceName, T::kInterfaceVersion, nullptr));
  }
};

/**
 * Lifecycle of a module. Every module DLL serves this as "rex.module" version 1.
 */
class IModule {
 public:
  REX_INTERFACE(IModule, "rex.module", 1);

  virtual ~IModule() = default;

  /**
   * @return The module's DLL base name, for example "rexgpu-xenos".
   */
  virtual const char* Name() const = 0;

  /**
   * @return Interfaces this module needs from modules listed before it, as "name@version".
   */
  virtual std::span<const char* const> RequiredInterfaces() const = 0;

  /**
   * Acquires pointers and registers descriptors. Allocates nothing that needs another
   * module initialized.
   *
   * @param factory Serves every loaded module's interfaces.
   * @param config The consumer's configuration. The module stores a pointer to its slice.
   * @return Ok, or an error that aborts Init before any module's Init runs.
   */
  virtual Status Connect(IInterfaceFactory* factory, const Config& config) = 0;

  /**
   * Releases what Connect acquired. Cannot fail.
   */
  virtual void Disconnect() = 0;

  /**
   * Does the module's real work using its config slice.
   *
   * @return Ok, or an error that unwinds every module initialized before it.
   */
  virtual Status Init() = 0;

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
  uint32_t version; /**< Served version. */
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
using CreateInterfaceFn = void* (*)(const char* name, uint32_t version, int32_t* out_status);

/**
 * Signature of rex_enumerate_interfaces.
 */
using EnumerateInterfacesFn = void (*)(const InterfaceEntry** out_entries, uint32_t* out_count);

}  // namespace rex
