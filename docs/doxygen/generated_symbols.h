/**
 * @file        generated_symbols.h
 * @brief       Symbols that generated code and module libraries export
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Documentation only. The templates `codegen/init_cpp.inja`, `codegen/register_cpp.inja` and
 * `codegen/module_registry_cpp.inja` write the real definitions, and nothing includes this file.
 */

#pragma once

/**
 * The image of a binary, defined in `<p>_init.cpp`. The host program passes it to the application.
 */
extern const rex::ppc::PPCImage kPPCImageConfig;

/**
 * The argument registers that each function reads at entry, r3 to r10 and f1 to f13, ending with
 * an entry of zeros.
 */
extern const rex::ppc::PPCCallUsage kPPCCallUsage[];

/**
 * Registers the recompiled functions of a DLL module. Defined in `<p>_register.cpp`.
 *
 * @param registrar The registrar that receives the function mappings.
 */
extern "C" void ReXRegisterModule(rex::kernel::IFunctionRegistrar* registrar);

/**
 * Returns the image of a DLL module. Defined in `<p>_register.cpp` of a DLL module.
 *
 * @return The module image.
 */
extern "C" const rex::ppc::PPCImage* ReXGetModuleImage();

/**
 * Registers every DLL module of the project with the module registry. Defined in
 * `module_registry.cpp` of the entrypoint folder.
 *
 * @param modules The module registry.
 */
void RegisterRecompiledModules(rex::kernel::IModuleRegistry& modules);

/**
 * Returns the identity of the SDK build that a module was compiled against. `REX_DECLARE_MODULE`
 * defines it, and the loader loads the module only when the identity equals its own.
 *
 * @return The build identity string.
 */
extern "C" const char* ReXBuildIdentity();

/**
 * Returns the interface pointer for a name and version. `REX_DECLARE_MODULE` defines it.
 *
 * @param name The interface name.
 * @param version The interface version.
 * @return The interface pointer, or null when the module serves no such interface.
 */
extern "C" rex::IInterface* ReXCreateInterface(const char* name, rex::u32 version);

/**
 * Lists the interfaces that a module serves, as `REX_MODULE_SERVES` declares them.
 * `REX_DECLARE_MODULE` defines it.
 *
 * @param out Receives the descriptions, or null to only count.
 * @param capacity The number of descriptions `out` holds.
 * @return The number of interfaces the module serves.
 */
extern "C" rex::u32 ReXEnumerateInterfaces(rex::InterfaceType* out, rex::u32 capacity);
