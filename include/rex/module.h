/**
 * @file        module.h
 * @brief       Interfaces, factory and module declaration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>
#include <rex/version.h>

namespace rex {

namespace detail {

template <typename T>
constexpr const char* FunctionSignature() {
#if defined(__clang__) || defined(__GNUC__)
  return __PRETTY_FUNCTION__;
#else
  return __FUNCSIG__;
#endif
}

template <typename T>
constexpr std::string_view ExtractTypeName() {
  std::string_view signature = FunctionSignature<T>();
#if defined(__clang__) || defined(__GNUC__)
  constexpr std::string_view kMarker = "T = ";
  const size_t begin = signature.find(kMarker) + kMarker.size();
  const size_t end = signature.find_first_of(";]", begin);
#else
  constexpr std::string_view kMarker = "FunctionSignature<";
  const size_t begin = signature.find(kMarker) + kMarker.size();
  const size_t end = signature.rfind(">(");
#endif
  std::string_view name = signature.substr(begin, end - begin);
  for (std::string_view prefix :
       {std::string_view("class "), std::string_view("struct "), std::string_view("enum ")}) {
    if (name.starts_with(prefix)) {
      name.remove_prefix(prefix.size());
    }
  }
  return name;
}

template <typename T>
inline constexpr auto kTypeNameChars = [] {
  constexpr std::string_view kName = ExtractTypeName<T>();
  std::array<char, kName.size() + 1> chars{};
  for (size_t i = 0; i < kName.size(); ++i) {
    chars[i] = kName[i];
  }
  return chars;
}();

/** The qualified name of T as a null terminated string, the same text on every compiler. */
template <typename T>
inline constexpr const char* kTypeName = kTypeNameChars<T>.data();

}  // namespace detail

/** The name and version of an interface. Lookups match the version exactly. */
struct InterfaceType {
  const char* name; /**< The qualified type name, such as rex::kernel::IObjectTable. */
  u32 version;
  bool is_private = false; /**< Only a private factory finds it. */
};

/** Base of every versioned interface. */
class IInterface : NonCopyable {
 public:
  virtual ~IInterface() = default;
};

/**
 * Passes a class name through unchanged. A macro that declares a class writes the name through it,
 * because a class name cannot be put in parentheses.
 *
 * @param x The class name.
 */
#define REX_DETAIL_ID(x) x

/**
 * Opens an interface class that derives from IInterface and carries its identity as kInterface.
 * The name is the qualified type name the compiler gives, so declare the class in a named
 * namespace. The class body, the closing brace and the semicolon follow the macro.
 *
 * @param version The version of the interface layout.
 *
 * @code
 * namespace rex::example {
 * REX_DECLARE_INTERFACE(IExample, 1)
 *   virtual int GetValue() const = 0;
 * };
 * }  // namespace rex::example
 * @endcode
 */
#define REX_DECLARE_INTERFACE(Name, version)             \
  class REX_DETAIL_ID(Name) : public ::rex::IInterface { \
   public:                                               \
    static constexpr ::rex::InterfaceType kInterface{::rex::detail::kTypeName<Name>, version};

/**
 * Opens an interface like REX_DECLARE_INTERFACE that only the app and the kernel can reach. The
 * public factory and AppInterfaces::Get never return it.
 *
 * @param version The version of the interface layout.
 */
#define REX_DECLARE_PRIVATE_INTERFACE(Name, version)     \
  class REX_DETAIL_ID(Name) : public ::rex::IInterface { \
   public:                                               \
    static constexpr ::rex::InterfaceType kInterface{::rex::detail::kTypeName<Name>, version, true};

/** Finds the interfaces the host and the modules serve. */
class IInterfaceFactory {
 public:
  virtual ~IInterfaceFactory() = default;

  /**
   * @param version The version, matched exactly.
   * @return The interface, or null when nothing serves that name and version.
   */
  virtual IInterface* Find(std::string_view name, u32 version) = 0;

  /**
   * Finds the interface of every module that serves it, in list order.
   *
   * @param version The version, matched exactly.
   * @return The interfaces, empty when nothing serves that name and version.
   */
  virtual std::vector<IInterface*> FindAll(std::string_view name, u32 version) = 0;

  /**
   * Finds an interface by type.
   *
   * @tparam T An interface declared with REX_DECLARE_INTERFACE.
   * @return The interface, or null when nothing serves its name and version.
   */
  template <typename T>
  T* Get() {
    return static_cast<T*>(Find(T::kInterface.name, T::kInterface.version));
  }

  /**
   * Finds the interface of every module that serves it, by type.
   *
   * @tparam T An interface declared with REX_DECLARE_INTERFACE.
   * @return The interfaces in list order.
   */
  template <typename T>
  std::vector<T*> FindAll() {
    std::vector<T*> found;
    for (IInterface* iface : FindAll(T::kInterface.name, T::kInterface.version)) {
      found.push_back(static_cast<T*>(iface));
    }
    return found;
  }
};

/** A unit of the SDK or of a title. It connects to the interfaces it needs and serves its own. */
REX_DECLARE_INTERFACE(IModule, 1)
  virtual std::string_view GetName() const = 0;

  /**
   * Lists the interfaces that must be served before this module connects.
   *
   * @return The required interfaces. Empty by default.
   */
  virtual std::span<const InterfaceType> GetRequiredInterfaces() const { return {}; }

  /**
   * Lists the interfaces this module serves besides IModule. REX_MODULE_SERVES defines it.
   *
   * @return The served interfaces. Empty by default.
   */
  virtual std::span<const InterfaceType> GetServedInterfaces() const { return {}; }

  /**
   * Connects the module to the interfaces it uses. At Connect the factory finds only the host
   * interfaces and those of earlier modules. From Init on it finds every connected module's
   * interfaces.
   *
   * @return Success, or the reason the module cannot work.
   */
  [[nodiscard]] virtual Result<void> Connect(IInterfaceFactory& factory) = 0;

  /** Releases what Connect took. Runs in reverse list order. */
  virtual void Disconnect() = 0;

  /**
   * Starts the module after every module connected.
   *
   * @return Success, or the reason the module cannot start.
   */
  [[nodiscard]] virtual Result<void> Init() = 0;

  /** Runs in reverse list order. */
  virtual void Shutdown() = 0;

  /**
   * Finds an interface the module serves besides IModule.
   *
   * @param version The version, matched exactly.
   * @return The interface, or null when the module serves none by that name and version.
   */
  virtual IInterface* QueryInterface(std::string_view name, u32 version) {
    (void)name;
    (void)version;
    return nullptr;
  }
};

/**
 * Finds an interface among those a module class derives from, for QueryInterface.
 *
 * @tparam Interfaces The interfaces the module serves besides IModule.
 * @param version The version, matched exactly.
 * @return The interface, or null when the module serves none by that name and version.
 */
template <typename... Interfaces, typename Module>
IInterface* FindModuleInterface(Module& module, std::string_view name, u32 version) {
  IInterface* found = nullptr;
  ((!found && name == Interfaces::kInterface.name && version == Interfaces::kInterface.version
        ? (found = static_cast<Interfaces*>(&module))
        : nullptr),
   ...);
  return found;
}

namespace detail {

/**
 * Returns the factory of the running app, or null before the app connects its modules and after it
 * shuts them down.
 */
REX_API IInterfaceFactory* GetAppInterfaceFactory();

}  // namespace detail

/** Reaches the interfaces of the running app from anywhere in the program. */
class AppInterfaces {
 public:
  /**
   * Finds an interface of the running app.
   *
   * @tparam T An interface declared with REX_DECLARE_INTERFACE.
   * @return The interface, or null when no app is running or nothing serves it.
   */
  template <typename T>
  static T* Get() {
    IInterfaceFactory* factory = detail::GetAppInterfaceFactory();
    return factory ? factory->Get<T>() : nullptr;
  }
};

#define REX_INTERFACE(T) ::rex::AppInterfaces::Get<T>()

/** The symbol a module exports to hand over its interfaces. */
inline constexpr const char* kCreateInterfaceSymbol = "ReXCreateInterface";

/** The symbol a module exports to list its interfaces. */
inline constexpr const char* kEnumerateInterfacesSymbol = "ReXEnumerateInterfaces";

/** The symbol a module exports to report the SDK build it was compiled against. */
inline constexpr const char* kBuildIdentitySymbol = "ReXBuildIdentity";

/** The identity of the SDK build these headers belong to. A module loads only when it matches. */
inline constexpr const char* kBuildIdentity = REXGLUE_BUILD_IDENTITY;

/** The type of ReXCreateInterface. Returns null when the module serves no such interface. */
using CreateInterfaceFn = IInterface* (*)(const char* name, u32 version);

/** The type of ReXEnumerateInterfaces. Returns the total count and writes at most capacity. */
using EnumerateInterfacesFn = u32 (*)(InterfaceType* out, u32 capacity);

namespace detail {

/**
 * The descriptions of the interfaces a module serves.
 *
 * @tparam Interfaces The interfaces, each declared with REX_DECLARE_INTERFACE or
 * REX_DECLARE_PRIVATE_INTERFACE.
 */
template <typename... Interfaces>
struct ServedTypeList {
  static constexpr InterfaceType kList[] = {Interfaces::kInterface...};
};

/** Lists every interface a module serves, IModule first. */
inline std::vector<InterfaceType> ListServedInterfaces(const IModule& module) {
  std::vector<InterfaceType> types{IModule::kInterface};
  for (const InterfaceType& type : module.GetServedInterfaces()) {
    if (std::string_view(type.name) != IModule::kInterface.name) {
      types.push_back(type);
    }
  }
  return types;
}

template <typename Module>
IInterface* CreateModuleInterface(Module& module, std::string_view name, u32 version) {
  if (name == IModule::kInterface.name && version == IModule::kInterface.version) {
    return static_cast<IModule*>(&module);
  }
  return module.QueryInterface(name, version);
}

inline u32 EnumerateModuleInterfaces(const IModule& module, InterfaceType* out, u32 capacity) {
  const std::vector<InterfaceType> types = ListServedInterfaces(module);
  const u32 count = static_cast<u32>(types.size());
  for (u32 i = 0; i < count && i < capacity; ++i) {
    out[i] = types[i];
  }
  return count;
}

}  // namespace detail
}  // namespace rex

/**
 * Declares the interfaces a module class serves besides IModule. Use once in a public section of
 * the class body.
 *
 * @code
 * class MyModule final : public rex::IModule {
 *  public:
 *   REX_MODULE_SERVES(IMyService)
 * };
 * @endcode
 *
 * @param ... The interfaces, each declared with REX_DECLARE_INTERFACE or
 * REX_DECLARE_PRIVATE_INTERFACE.
 */
#define REX_MODULE_SERVES(...)                                                   \
  ::std::span<const ::rex::InterfaceType> GetServedInterfaces() const override { \
    return ::rex::detail::ServedTypeList<__VA_ARGS__>::kList;                    \
  }

/**
 * Exports the module identity and its interfaces. Use once per module.
 *
 * @code
 * REX_DECLARE_MODULE(MyModule)
 * @endcode
 *
 * @param ModuleClass A class deriving from IModule, with a default constructor. One instance exists
 * as long as the library.
 */
#define REX_DECLARE_MODULE(ModuleClass)                                                           \
  namespace {                                                                                     \
  using ReXModuleType = ModuleClass;                                                              \
  ReXModuleType& ReXModuleInstance() {                                                            \
    static ModuleClass instance;                                                                  \
    return instance;                                                                              \
  }                                                                                               \
  }                                                                                               \
  extern "C" REX_DETAIL_EXPORT const char* ReXBuildIdentity() {                                   \
    return REXGLUE_BUILD_IDENTITY;                                                                \
  }                                                                                               \
  extern "C" REX_DETAIL_EXPORT ::rex::IInterface* ReXCreateInterface(const char* name,            \
                                                                     ::rex::u32 version) {        \
    return ::rex::detail::CreateModuleInterface<ModuleClass>(ReXModuleInstance(), name, version); \
  }                                                                                               \
  extern "C" REX_DETAIL_EXPORT ::rex::u32 ReXEnumerateInterfaces(::rex::InterfaceType* out,       \
                                                                 ::rex::u32 capacity) {           \
    return ::rex::detail::EnumerateModuleInterfaces(ReXModuleInstance(), out, capacity);          \
  }
