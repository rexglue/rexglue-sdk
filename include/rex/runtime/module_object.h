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

#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/filesystem/xex_types.h>
#include <rex/export.h>
#include <rex/platform/thread.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/module.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

class Memory;

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
enum class ModuleType : u8 { kKernel, kUser };

/** The container format of a loaded user module. */
enum class ModuleFormat : u8 { kUndefined, kXEX, kELF };

/** The launch request state that the XAM loader exports keep on the XAM module. */
struct XAMLoaderState {
  /** True once XamLoaderSetLaunchData stored a non empty block. */
  bool launch_data_present = false;

  /** Bytes the title passed to XamLoaderSetLaunchData, returned by XamLoaderGetLaunchData. */
  std::vector<u8> launch_data;
  u32 launch_flags = 0; /**< Flags the title passed to XamLoaderLaunchTitle. */
  /** Path the title passed to XamLoaderLaunchTitle, `game:\default.xex` when it passed none. */
  std::string launch_path;
};

/** The state only a kernel module has. */
struct KernelModuleState {
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
  XAMLoaderState loader_data; /**< Launch request state, used only by the XAM module. */
  /** 1 ms timer that refreshes the tick count of KeTimeStampBundle, set only on Xboxkrnl. */
  std::unique_ptr<rex::platform::HighResolutionTimer> timestamp_timer;
};

/** The state only a user module has. */
struct UserModuleState {
  /** Guest address of the copy of the XEX header, read by XEX header queries. */
  u32 guest_xex_header = 0;

  /** Container format detected when the module was loaded. */
  ModuleFormat module_format = ModuleFormat::kUndefined;

  /** True when the XEX header sets the DLL flag, so load and unload call the entry point. */
  bool is_dll_module = false;

  /** Guest address of the entry point from the XEX header or the ELF header. */
  u32 entry_point = 0;
  u32 stack_size = 0; /**< Default stack size in bytes, used for the main thread. */

  /** Guest path of the recompiled library registration the module loaded with, empty when none. */
  std::string recompiled_key;

  /** User modules this module imports from, each holding 1 load reference for it. */
  std::vector<X_HANDLE> dependencies;

  /** Thunk address made for each caller and ordinal, reused by later lookups. */
  std::unordered_map<KernelModuleState::ThunkKey, u32, KernelModuleState::ThunkKeyHash> thunk_cache;
};

/** The `KernelObject` data of a module. The kind is always known from type. */
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
  std::unique_ptr<rex::runtime::Module> processor_module;

  /** Guest address of the module X_LDR_DATA_TABLE_ENTRY, the HMODULE titles use. */
  u32 hmodule_ptr = 0;
  Runtime* runtime;         /**< Runtime that owns the module, used to free `hmodule_ptr`. */
  UserModuleState user;     /**< State of a user module, unused for a kernel module. */
  KernelModuleState kernel; /**< State of a kernel module, unused for a user module. */

  /**
   * Reports whether a module answers to a name.
   *
   * @param query A path, a file name or a module name, compared case insensitively.
   * @return True when the query matches the path, the file name or the name of the module.
   */
  REX_RUNTIME_API bool IsNamed(std::string_view query) const;

  /**
   * Finds a resource section of a module.
   *
   * @param section_name The section name.
   * @param out_section_data Receives the guest address of the section.
   * @param out_section_size Receives the size of the section.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for a kernel module.
   */
  REX_RUNTIME_API X_STATUS GetSection(std::string_view section_name, u32* out_section_data,
                                      u32* out_section_size) const;

  /**
   * Finds an exported function of a module. A kernel module returns a thunk.
   *
   * @param owning_runtime The runtime the call runs in.
   * @param ordinal The export ordinal.
   * @param caller_address The guest address of the caller, used to key kernel thunks.
   * @return The guest address, 0 when the module has no such export.
   */
  REX_RUNTIME_API u32 GetProcAddress(Runtime& owning_runtime, u16 ordinal, u32 caller_address = 0);

  /**
   * Finds an exported function of a module by name.
   *
   * @param export_name The export name.
   * @return The guest address, 0 when the module has no such export.
   */
  REX_RUNTIME_API u32 GetProcAddress(std::string_view export_name) const;

  /**
   * Returns the XEX image of a user module.
   *
   * @return The XEX module, null when the module is not an XEX.
   */
  REX_RUNTIME_API rex::runtime::XEXModule* GetXEX();

  /**
   * Returns the XEX image of a user module for reading.
   *
   * @return The XEX module, null when the module is not an XEX.
   */
  REX_RUNTIME_API const rex::runtime::XEXModule* GetXEX() const;

  /**
   * Returns the title ID of a user module.
   *
   * @return The title ID of the XEX header, 0 when the module is not an XEX or has none.
   */
  REX_RUNTIME_API u32 GetTitleId() const;

  /**
   * Loads a module image from a file and applies a sibling XEX patch when one exists.
   *
   * @param owning_runtime The runtime the call runs in.
   * @param guest_path The guest path of the file.
   * @return X_STATUS_SUCCESS, or the status of the first step that failed.
   */
  REX_RUNTIME_API X_STATUS LoadFromFile(Runtime& owning_runtime, std::string_view guest_path);

  /**
   * Loads a module image from memory.
   *
   * @param owning_runtime The runtime the call runs in.
   * @param addr The host address of the image.
   * @param length The size of the image.
   * @return X_STATUS_PENDING for an XEX that needs its second load step, X_STATUS_SUCCESS for an
   * ELF, or the failure status.
   */
  REX_RUNTIME_API X_STATUS LoadFromMemory(Runtime& owning_runtime, const void* addr, size_t length);

  /**
   * Unloads the image of a user module.
   *
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL when the image cannot be unloaded.
   */
  REX_RUNTIME_API X_STATUS Unload();

  /**
   * Finds an optional XEX header. For host use only.
   *
   * @param key The header key.
   * @param out_ptr Receives the header value or a pointer to its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  REX_RUNTIME_API X_STATUS GetOptHeader(filesystem::XEXHeaderKey key, void** out_ptr);

  /**
   * Finds an optional XEX header. For host use only.
   *
   * @tparam T The type of the value the header holds.
   * @param key The header key.
   * @param out_ptr Receives the header value or a pointer to its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  template <typename T>
  X_STATUS OptHeader(filesystem::XEXHeaderKey key, T* out_ptr) {
    return GetOptHeader(key, reinterpret_cast<void**>(out_ptr));
  }

  /**
   * Finds an optional XEX header and returns an address that the title can read.
   *
   * @param key The header key.
   * @param out_header_guest_ptr Receives the header value or the guest address of its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  REX_RUNTIME_API X_STATUS GetOptHeader(filesystem::XEXHeaderKey key,
                                        u32* out_header_guest_ptr) const;

  /**
   * Erases the cached thunks whose guest address falls in a range.
   *
   * @param lo The first address of the range.
   * @param hi The address after the last address of the range.
   */
  REX_RUNTIME_API void InvalidateThunkCache(u32 lo, u32 hi);

  /**
   * Binds a variable export to the guest address that holds it.
   *
   * @param ordinal The variable ordinal.
   * @param address The guest address of the variable.
   */
  void SetVariable(u16 ordinal, u32 address) { kernel.variables.Set(ordinal, address); }

  /**
   * Returns the address of a variable export.
   *
   * @param ordinal The variable ordinal.
   * @return The guest address, or 0 when the module never created the variable.
   */
  u32 GetVariable(u16 ordinal) const { return kernel.variables.Get(ordinal); }

  /**
   * Reads the handle that the loader data entry of an HMODULE holds.
   *
   * @param hmodule The host pointer of the loader data entry.
   * @return The handle stored in the entry.
   */
  REX_RUNTIME_API static u32 HandleFromHModule(void* hmodule);

  /**
   * Finds the module of an HMODULE.
   *
   * @param runtime The runtime the call runs in.
   * @param hmodule The host pointer of the loader data entry.
   * @return The module handle, 0 when the stored handle does not name a module.
   */
  REX_RUNTIME_API static X_HANDLE FromHModule(Runtime& runtime, void* hmodule);

  /**
   * Creates the `KernelObject` of a kernel module with its first handle.
   *
   * @param runtime The runtime the call runs in.
   * @param path The guest path of the module, its name is the file name of the path.
   * @return The handle with a count of 1, 0 when the table cannot grow.
   */
  REX_RUNTIME_API static X_HANDLE CreateKernelModule(Runtime& runtime, std::string_view path);

  /**
   * Creates the `KernelObject` of a user module with its first handle.
   *
   * @param runtime The runtime the call runs in.
   * @return The handle with a count of 1, 0 when the table cannot grow.
   */
  REX_RUNTIME_API static X_HANDLE CreateUserModule(Runtime& runtime);

  /**
   * Finds an optional header of an XEX header in memory and returns an address that the title can
   * read.
   *
   * @param memory The memory that holds the header.
   * @param header The XEX header.
   * @param key The header key.
   * @param out_header_guest_ptr Receives the header value or the guest address of its data.
   * @return X_STATUS_SUCCESS, or X_STATUS_NOT_FOUND.
   */
  REX_RUNTIME_API static X_STATUS GetOptHeader(const runtime::Memory* memory,
                                               const filesystem::XEXHeader* header,
                                               filesystem::XEXHeaderKey key,
                                               u32* out_header_guest_ptr);
};

/**
 * Creates an `KernelObject` with its first handle and stores that handle in the loader data entry.
 *
 * @param runtime The runtime the call runs in.
 * @param type Whether the module is kernel or user.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
REX_RUNTIME_API X_HANDLE CreateModule(Runtime& runtime, ModuleType type);

}  // namespace rex::runtime
