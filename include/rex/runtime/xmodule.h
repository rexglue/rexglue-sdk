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

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/module.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** HMODULE points to this struct. */
struct X_LDR_DATA_TABLE_ENTRY {
  X_LIST_ENTRY in_load_order_links;
  X_LIST_ENTRY in_memory_order_links;
  X_LIST_ENTRY in_initialization_order_links;

  be_u32 dll_base;
  be_u32 image_base;
  be_u32 image_size;

  X_UNICODE_STRING full_dll_name;
  X_UNICODE_STRING base_dll_name;

  be_u32 flags;
  be_u32 full_image_size;
  be_u32 entry_point;
  be_u16 load_count;
  be_u16 module_index;
  be_u32 dll_base_original;
  be_u32 checksum; /**< Holds the kernel handle. */
  be_u32 load_flags;
  be_u32 time_date_stamp;
  be_u32 loaded_imports;
  be_u32 xex_header_base;
  be_u32 closure_root;
  be_u32 traversal_parent;
};
REX_STATIC_ASSERT_SIZE(X_LDR_DATA_TABLE_ENTRY, 0x64);

/** Module kind, HLE kernel code or a loaded title image. */
enum class ModuleType : u8 { Kernel, User };

/** The container format of a loaded user module. */
enum class ModuleFormat : u8 { Undefined, Xex, Elf };

/** The launch request state that the Xam loader exports keep on the Xam module. */
struct XamLoaderData {
  /** True once XamLoaderSetLaunchData stored a non empty block. */
  bool launch_data_present = false;
  /** Bytes the title passed to XamLoaderSetLaunchData, returned by XamLoaderGetLaunchData. */
  std::vector<u8> launch_data;
  u32 launch_flags = 0; /**< Flags the title passed to XamLoaderLaunchTitle. */
  /** Path the title passed to XamLoaderLaunchTitle, `game:\default.xex` when it passed none. */
  std::string launch_path;
};

/** The state only a kernel module has. */
struct KernelModuleData {
  /** Identifies a thunk by the module that asked for it and the export ordinal. */
  struct ThunkKey {
    /** Base address of the module that called the export, 0 when no module contains the caller. */
    u32 caller_module_base;
    u16 ordinal; /**< Ordinal of the exported function. */
    bool operator==(const ThunkKey&) const = default;
  };

  /** Hashes a thunk key. */
  struct ThunkKeyHash {
    /**
     * @param key The key.
     * @return The hash.
     */
    REX_RUNTIME_API size_t operator()(const ThunkKey& key) const noexcept;
  };

  /** Thunk address made for each caller and ordinal, reused by later lookups. */
  std::unordered_map<ThunkKey, u32, ThunkKeyHash> thunk_cache;
  /** Guest addresses of the variable exports of the module, bound at module creation. */
  rex::runtime::ExportVariables variables;
  XamLoaderData loader_data; /**< Launch request state, used only by the Xam module. */
  /** 1 ms timer that refreshes the tick count of KeTimeStampBundle, set only on Xboxkrnl. */
  std::unique_ptr<rex::platform::HighResolutionTimer> timestamp_timer;
};

/** The state only a user module has. */
struct UserModuleData {
  /** Guest address of the copy of the XEX header, read by XEX header queries. */
  u32 guest_xex_header = 0;
  /** Container format detected when the module was loaded. */
  ModuleFormat module_format = ModuleFormat::Undefined;
  /** True when the XEX header sets the DLL flag, so load and unload call the entry point. */
  bool is_dll_module = false;
  /** Guest address of the entry point from the XEX header or the ELF header. */
  u32 entry_point = 0;
  u32 stack_size = 0; /**< Default stack size in bytes, used for the main thread. */
};

/** The `ObjectRecord` data of a module. The kind is always known from type. */
struct ModuleObject : public rex::NonCopyable {
  /**
   * Allocates the loader data entry.
   *
   * @param runtime_in The runtime the module belongs to.
   * @param type_in Whether the module is kernel or user.
   */
  REX_RUNTIME_API ModuleObject(Runtime* runtime_in, ModuleType type_in);
  /** Unloads a user module and frees the loader data entry. */
  REX_RUNTIME_API ~ModuleObject();

  ModuleType type;  /**< Selects whether `kernel` or `user` holds the state. */
  std::string name; /**< Module name, the file name of `path` for a user module. */
  std::string path; /**< Absolute path of the file a user module was loaded from. */
  /** Loaded XEX or ELF image of a user module, owned by this object, null until loaded. */
  rex::runtime::Module* processor_module = nullptr;
  /** Guest address of the module X_LDR_DATA_TABLE_ENTRY, the HMODULE titles use. */
  u32 hmodule_ptr = 0;
  Runtime* runtime;        /**< Runtime that owns the module, used to free `hmodule_ptr`. */
  UserModuleData user;     /**< State of a user module, unused for a kernel module. */
  KernelModuleData kernel; /**< State of a kernel module, unused for a user module. */
};

/**
 * Creates an `ObjectRecord` with its first handle and stores that handle in the loader data entry.
 *
 * @param runtime The runtime the call runs in.
 * @param type Whether the module is kernel or user.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
REX_RUNTIME_API X_HANDLE CreateModule(Runtime& runtime, ModuleType type);

/**
 * Reports whether a module answers to a name.
 *
 * @param module The module.
 * @param name A path, a file name or a module name, compared case insensitively.
 * @return True when the name matches the path, the file name or the name of the module.
 */
REX_RUNTIME_API bool ModuleMatches(const ModuleObject& module, std::string_view name);

/**
 * Finds a resource section of a module.
 *
 * @param module The module.
 * @param name The section name.
 * @param out_section_data Receives the guest address of the section.
 * @param out_section_size Receives the size of the section.
 * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for a kernel module.
 */
REX_RUNTIME_API X_STATUS ModuleGetSection(const ModuleObject& module, std::string_view name,
                                          u32* out_section_data, u32* out_section_size);

/**
 * Reads the handle that the loader data entry of an HMODULE holds.
 *
 * @param hmodule The host pointer of the loader data entry.
 * @return The handle stored in the entry.
 */
REX_RUNTIME_API u32 ModuleHandleFromHModule(void* hmodule);

/**
 * Finds the module of an HMODULE.
 *
 * @param runtime The runtime the call runs in.
 * @param hmodule The host pointer of the loader data entry.
 * @return The module handle, 0 when the stored handle does not name a module.
 */
REX_RUNTIME_API X_HANDLE ModuleFromHModule(Runtime& runtime, void* hmodule);

/**
 * Finds an exported function of a module. A kernel module returns a thunk.
 *
 * @param runtime The runtime the call runs in.
 * @param module The module.
 * @param ordinal The export ordinal.
 * @param caller_address The guest address of the caller, used to key kernel thunks.
 * @return The guest address, 0 when the module has no such export.
 */
REX_RUNTIME_API u32 ModuleGetProcAddressByOrdinal(Runtime& runtime, ModuleObject& module,
                                                  u16 ordinal, u32 caller_address = 0);

/**
 * Finds an exported function of a module by name.
 *
 * @param module The module.
 * @param name The export name.
 * @return The guest address, 0 when the module has no such export.
 */
REX_RUNTIME_API u32 ModuleGetProcAddressByName(ModuleObject& module, std::string_view name);

}  // namespace rex::runtime
