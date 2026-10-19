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

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/version.h>

namespace rex {

struct Config;

/**
 * Result of an interface request at the module boundary.
 */
// NOLINTNEXTLINE(performance-enum-size): the C export reports the status as i32
enum class InterfaceStatus : i32 {
  kOk = 0,              /**< The interface is served. */
  kNotFound = 1,        /**< No interface of that name. */
  kVersionMismatch = 2, /**< The name is served at a different version. */
};

/** Base of every interface class. Holds the virtual destructor and forbids copies. */
class IInterface : public NonCopyable {
 public:
  virtual ~IInterface() = default;
};

/**
 * A string literal usable as a template argument.
 *
 * @tparam N The literal size including the terminator.
 */
template <std::size_t N>
struct InterfaceName {
  char value[N]{}; /**< The characters including the terminator. */

  /**
   * Copies a string literal.
   *
   * @param text The literal.
   */
  constexpr InterfaceName(const char (&text)[N]) { std::copy_n(text, N, value); }
};

/**
 * Base of an interface served through the module ABI.
 *
 * @tparam Name The dotted interface name.
 * @tparam Version The interface version.
 */
template <InterfaceName Name, u32 Version>
class Interface : public IInterface {
 public:
  static constexpr const char* kInterfaceName = Name.value; /**< The dotted interface name. */
  static constexpr u32 kInterfaceVersion = Version;         /**< The interface version. */
};

/**
 * Joins 2 tokens after their arguments are left unexpanded, so a class name never stands alone in
 * a macro body.
 *
 * @param a The left token.
 * @param b The right token, may be empty.
 * @code
 * REX_DETAIL_PASTE(IFileSystem, )
 * @endcode
 */
#define REX_DETAIL_PASTE(a, b) a##b
/**
 * Picks the macro that matches the argument count of REX_INTERFACE.
 *
 * @param _1 The first argument.
 * @param _2 The second argument.
 * @param _3 The third argument.
 * @param Macro The macro to expand.
 * @param ... The remaining arguments.
 * @code
 * REX_DETAIL_INTERFACE_PICK(A, B, C, REX_DETAIL_INTERFACE_3, REX_DETAIL_INTERFACE_2,
 * REX_DETAIL_INTERFACE_1)
 * @endcode
 */
#define REX_DETAIL_INTERFACE_PICK(_1, _2, _3, Macro, ...) Macro
/**
 * Opens an interface class that derives from rex::IInterface.
 *
 * @param Name The interface class name.
 * @code
 * REX_DETAIL_INTERFACE_1(IPlain)
 * @endcode
 */
#define REX_DETAIL_INTERFACE_1(Name) class REX_DETAIL_PASTE(Name, ) : public ::rex::IInterface {
/**
 * Opens an interface class that derives from rex::Interface.
 *
 * @param Name The interface class name.
 * @param name The dotted interface name, a string literal.
 * @param version The interface version, an integer literal.
 * @code
 * REX_DETAIL_INTERFACE_3(IExample, "rex.example", 1)
 * @endcode
 */
#define REX_DETAIL_INTERFACE_3(Name, name, version) \
  class REX_DETAIL_PASTE(Name, ) : public ::rex::Interface<name, version> {
/**
 * Opens an interface class and its brace. The access label, the class body, the closing brace and
 * the semicolon follow the macro.
 * The 3 argument form derives from rex::Interface and serves kInterfaceName and kInterfaceVersion.
 * The 1 argument form derives from rex::IInterface.
 *
 * @param ... The interface class name, then in the 3 argument form the dotted interface name as a
 *        string literal and the interface version as an integer literal.
 *
 * @code
 * REX_INTERFACE(IExample, "rex.example", 1)
 *  public:
 *   virtual Result<void> Mount(const MountEntry& entry) = 0;
 * };
 * @endcode
 */
#define REX_INTERFACE(...)                                                               \
  REX_DETAIL_INTERFACE_PICK(__VA_ARGS__, REX_DETAIL_INTERFACE_3, REX_DETAIL_INTERFACE_2, \
                            REX_DETAIL_INTERFACE_1)(__VA_ARGS__)

/**
 * Serves interfaces by name and version.
 */
REX_INTERFACE(IInterfaceFactory)
 public:
  /**
   * Returns an interface.
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
 * Lifecycle of a module.
 */
REX_INTERFACE(IModule, "rex.module", 1)
 public:
  /**
   * Returns the module's DLL base name.
   *
   * @return The DLL base name, for example "rexgpu-xenos".
   */
  virtual const char* GetName() const = 0;

  /**
   * Returns the interfaces this module needs from modules listed before it.
   *
   * @return The required interfaces, each as "name@version".
   */
  virtual std::span<const char* const> GetRequiredInterfaces() const = 0;

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
inline constexpr const char* kCreateInterfaceSymbol = "ReXCreateInterface";

/**
 * Name of the C export that returns the served interface table.
 */
inline constexpr const char* kEnumerateInterfacesSymbol = "ReXEnumerateInterfaces";

/**
 * Name of the C export that returns the SDK build identity a module was compiled against.
 */
inline constexpr const char* kBuildIdentitySymbol = "ReXBuildIdentity";

/**
 * Identity of the SDK build these headers belong to. A module loads only when its exported
 * identity equals this value.
 */
inline constexpr const char* kBuildIdentity = REXGLUE_BUILD_IDENTITY;

/**
 * Signature of ReXBuildIdentity.
 */
using BuildIdentityFn = const char* (*)();

/**
 * Signature of ReXCreateInterface.
 */
using CreateInterfaceFn = void* (*)(const char* name, u32 version, i32* out_status);

/**
 * Signature of ReXEnumerateInterfaces.
 */
using EnumerateInterfacesFn = void (*)(const InterfaceEntry** out_entries, u32* out_count);

}  // namespace rex

/**
 * Generates the 3 C exports of a module DLL for a class that declares
 * `static constexpr std::array<rex::InterfaceEntry, N> kInterfaces`.
 *
 * Called exactly once per module DLL at namespace scope. `ReXCreateInterface(name, version,
 * out_status)` searches the table and reports Ok, NotFound or VersionMismatch,
 * `ReXEnumerateInterfaces(out_entries, out_count)` returns the table and `ReXBuildIdentity()`
 * returns the identity of the SDK build the module was compiled against. Nothing runs at DLL
 * load, the module instance is created by the table's getters on first request.
 *
 * @param ModuleClass The module class holding the interface table.
 *
 * @code
 * REX_DECLARE_MODULE(rex::gpu::XenosModule)
 * @endcode
 */
#define REX_DECLARE_MODULE(ModuleClass)                                                       \
  extern "C" REX_DETAIL_EXPORT const char* ReXBuildIdentity() {                               \
    return ::rex::kBuildIdentity;                                                             \
  }                                                                                           \
  extern "C" REX_DETAIL_EXPORT void* ReXCreateInterface(const char* name, ::rex::u32 version, \
                                                        ::rex::i32* out_status) {             \
    if (!name) {                                                                              \
      if (out_status)                                                                         \
        *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::kNotFound);             \
      return nullptr;                                                                         \
    }                                                                                         \
    for (const ::rex::InterfaceEntry& entry : ModuleClass::kInterfaces) {                     \
      if (std::strcmp(entry.name, name) != 0)                                                 \
        continue;                                                                             \
      if (entry.version != version) {                                                         \
        if (out_status)                                                                       \
          *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::kVersionMismatch);    \
        return nullptr;                                                                       \
      }                                                                                       \
      if (out_status)                                                                         \
        *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::kOk);                   \
      return entry.get();                                                                     \
    }                                                                                         \
    if (out_status)                                                                           \
      *out_status = static_cast<::rex::i32>(::rex::InterfaceStatus::kNotFound);               \
    return nullptr;                                                                           \
  }                                                                                           \
  extern "C" REX_DETAIL_EXPORT void ReXEnumerateInterfaces(                                   \
      const ::rex::InterfaceEntry** out_entries, ::rex::u32* out_count) {                     \
    if (out_entries)                                                                          \
      *out_entries = ModuleClass::kInterfaces.data();                                         \
    if (out_count)                                                                            \
      *out_count = static_cast<::rex::u32>(ModuleClass::kInterfaces.size());                  \
  }
