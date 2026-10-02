#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string_view>

#include <rex/filesystem/xex_types.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xmodule.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/**
 * Creates the record of a user module with its first handle.
 *
 * @param runtime The runtime the call runs in.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
X_HANDLE CreateUserModule(Runtime& runtime);

/**
 * Returns the XEX image of a user module.
 *
 * @param module A user module that holds an XEX image.
 * @return The XEX module.
 */
rex::runtime::XexModule* UserModuleXEX(const ModuleObject& module);

/**
 * Returns the title ID of a user module.
 *
 * @param module A user module.
 * @return The title ID of the XEX header, 0 when the module is not an XEX or has none.
 */
u32 UserModuleTitleID(const ModuleObject& module);

/**
 * Loads a module image from a file and applies a sibling XEX patch when one exists.
 *
 * @param runtime The runtime the call runs in.
 * @param module A user module that holds no image yet.
 * @param path The guest path of the file.
 * @return X_STATUS_SUCCESS, or the status of the first step that failed.
 */
X_STATUS UserModuleLoadFromFile(Runtime& runtime, ModuleObject& module, std::string_view path);

/**
 * Loads a module image from memory.
 *
 * @param runtime The runtime the call runs in.
 * @param module A user module that holds no image yet.
 * @param addr The host address of the image.
 * @param length The size of the image.
 * @return X_STATUS_PENDING for an XEX that needs its second load step, X_STATUS_SUCCESS for an ELF,
 *         or the failure status.
 */
X_STATUS UserModuleLoadFromMemory(Runtime& runtime, ModuleObject& module, const void* addr,
                                  size_t length);

/**
 * Unloads the image of a user module.
 *
 * @param module A user module.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL when the image cannot be unloaded.
 */
X_STATUS UserModuleUnload(ModuleObject& module);

/**
 * Finds an optional XEX header. For host use only.
 *
 * @param module A user module.
 * @param key The header key.
 * @param out_ptr Receives the header value or a pointer to its data.
 * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
 */
X_STATUS UserModuleGetOptHeader(ModuleObject& module, xex2_header_keys key, void** out_ptr);

/**
 * Finds an optional XEX header. For host use only.
 *
 * @tparam T The type of the value the header holds.
 * @param module A user module.
 * @param key The header key.
 * @param out_ptr Receives the header value or a pointer to its data.
 * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
 */
template <typename T>
X_STATUS UserModuleGetOptHeader(ModuleObject& module, xex2_header_keys key, T* out_ptr) {
  return UserModuleGetOptHeader(module, key, reinterpret_cast<void**>(out_ptr));
}

/**
 * Finds an optional XEX header and returns an address that recompiled code can read.
 *
 * @param module A user module.
 * @param key The header key.
 * @param out_header_guest_ptr Receives the header value or the guest address of its data.
 * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
 */
X_STATUS UserModuleGetOptHeader(ModuleObject& module, xex2_header_keys key,
                                u32* out_header_guest_ptr);

/**
 * Finds an optional header of an XEX header in memory and returns an address that recompiled code
 * can read.
 *
 * @param memory The guest memory.
 * @param header The XEX header.
 * @param key The header key.
 * @param out_header_guest_ptr Receives the header value or the guest address of its data.
 * @return X_STATUS_SUCCESS, or X_STATUS_NOT_FOUND.
 */
X_STATUS UserModuleGetOptHeader(const runtime::Memory* memory, const xex2_header* header,
                                xex2_header_keys key, u32* out_header_guest_ptr);

}  // namespace rex::runtime
