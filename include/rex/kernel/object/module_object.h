/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/module_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/kernel/types/status.h>
#include <rex/platform.h>
#include <rex/assert.h>
#include <rex/kernel/types/xex.h>
#include <rex/platform/thread.h>
#include <rex/utility.h>
#include <rex/platform/timer.h>
#include <rex/kernel/export.h>
#include <rex/kernel/module.h>
#include <rex/kernel/types/io.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/types/list.h>
#include <rex/types.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

class Memory;

/** HMODULE points to this struct. */
struct X_LDR_DATA_TABLE_ENTRY {
  X_LIST_ENTRY in_load_order_links;
  /** Link in the loader list in memory order. */
  X_LIST_ENTRY in_memory_order_links;
  /** Link in the loader list in initialization order. */
  X_LIST_ENTRY in_initialization_order_links;

  be_u32 dll_base; /**< Set to 0 when a user module loads. */
  be_u32 image_base;
  be_u32 image_size;

  X_UNICODE_STRING full_dll_name;
  X_UNICODE_STRING base_dll_name;

  be_u32 flags;
  be_u32 full_image_size; /**< Size of the image in bytes from the XEX security info. */
  be_u32 entry_point;
  be_u16 load_count; /**< Number of load references, 1 when a user module loads. */
  be_u16 module_index;
  be_u32 dll_base_original;
  be_u32 checksum; /**< Holds the kernel handle. */
  be_u32 load_flags;
  be_u32 time_date_stamp;
  be_u32 loaded_imports;
  be_u32 xex_header_base; /**< Guest address of the copy of the XEX header. */
  be_u32 closure_root;
  be_u32 traversal_parent;
};
REX_STATIC_ASSERT_SIZE(X_LDR_DATA_TABLE_ENTRY, 0x64);

/** Module type, HLE kernel code or a loaded title image. */
enum class ModuleType : u8 { kKernel, kUser };

enum class ModuleFormat : u8 { kUndefined, kXEX, kELF };

struct KernelModuleState {
  /** Identifies a thunk by the module that asked for it and the export ordinal. */
  struct ThunkKey {
    /** Base address of the module that called the export, 0 when no module contains the caller. */
    u32 caller_module_base;
    u16 ordinal;
    bool operator==(const ThunkKey&) const = default;
  };

  struct ThunkKeyHash {
    REX_KERNEL_API size_t operator()(const ThunkKey& key) const noexcept;
  };

  /** Thunk address made for each caller and ordinal, reused by later lookups. */
  std::unordered_map<ThunkKey, u32, ThunkKeyHash> thunk_cache;

  /** Guest addresses of the variable exports of the module, bound at module creation. */
  rex::kernel::ExportVariableTable variables;
  /** 1 ms timer that refreshes the tick count of KeTimeStampBundle, set only on the main kernel
   * module. */
  std::unique_ptr<rex::platform::HighResolutionTimer> timestamp_timer;
};

struct UserModuleState {
  /** Guest address of the copy of the XEX header, read by XEX header queries. */
  u32 guest_xex_header = 0;

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

/** What XAM reads from a loaded module. */
class ILoadedModule {
 public:
  virtual ~ILoadedModule() = default;

  /**
   * Returns the absolute path of the file a user module was loaded from.
   */
  [[nodiscard]] virtual const std::string& GetPath() const = 0;

  /**
   * Finds a resource section of a module.
   *
   * @param out_section_data Receives the guest address of the section.
   * @param out_section_size Receives the size of the section.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for a kernel module.
   */
  virtual X_STATUS GetSection(std::string_view section_name, u32* out_section_data,
                              u32* out_section_size) const = 0;

  /** Returns the title ID of a user module, 0 when the module is not an XEX or has none. */
  [[nodiscard]] virtual u32 GetTitleID() const = 0;

  /**
   * Lists the alternate title IDs of the XEX header of a user module.
   *
   * @return The title IDs, empty when the module is not an XEX.
   */
  [[nodiscard]] virtual std::vector<u32> GetAlternateTitleIDs() const = 0;

  /**
   * Finds an optional XEX header. For host use only.
   *
   * @param out_ptr Receives the header value or a pointer to its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  virtual X_STATUS GetOptHeader(XEXHeaderKey key, void** out_ptr) = 0;

  /**
   * Finds an optional XEX header. For host use only.
   *
   * @tparam T The type of the value the header holds.
   * @param out_ptr Receives the header value or a pointer to its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  template <typename T>
  X_STATUS OptHeader(XEXHeaderKey key, T* out_ptr) {
    return GetOptHeader(key, reinterpret_cast<void**>(out_ptr));
  }
};

struct ModuleObject : public ILoadedModule, public rex::NonCopyable {
  /**
   * Allocates the loader data entry.
   *
   * @param type_in Whether the module is kernel or user.
   */
  REX_KERNEL_API ModuleObject(TitleRuntime* runtime_in, ModuleType type_in);

  /** Unloads a user module and frees the loader data entry. */
  REX_KERNEL_API ~ModuleObject();

  ModuleType type;  /**< Selects whether `kernel` or `user` holds the state. */
  std::string name; /**< Module name, the file name of `path` for a user module. */
  std::string path; /**< Absolute path of the file a user module was loaded from. */
  /** Loaded XEX or ELF image of a user module, owned by this object, null until loaded. */
  PropagateConst<std::unique_ptr<rex::kernel::Module>> processor_module;

  /** Guest address of the module X_LDR_DATA_TABLE_ENTRY, the HMODULE titles use. */
  u32 hmodule_ptr = 0;
  TitleRuntime* runtime;    /**< TitleRuntime that owns the module, used to free `hmodule_ptr`. */
  UserModuleState user;     /**< State of a user module, unused for a kernel module. */
  KernelModuleState kernel; /**< State of a kernel module, unused for a user module. */

  /**
   * Reports whether a name matches the module.
   *
   * @param query A path, a file name or a module name, compared case insensitively.
   * @return True when the query matches the path, the file name or the name of the module.
   */
  REX_KERNEL_API bool IsNamed(std::string_view query) const;

  REX_KERNEL_API X_STATUS GetSection(std::string_view section_name, u32* out_section_data,
                                     u32* out_section_size) const override;

  /**
   * Finds an exported function of a module. A kernel module returns a thunk.
   *
   * @param caller_address The guest address of the caller, used to key kernel thunks.
   * @return The guest address, 0 when the module has no such export.
   */
  REX_KERNEL_API u32 GetExportAddress(TitleRuntime& owning_runtime, u16 ordinal,
                                      u32 caller_address = 0);

  /**
   * Finds an exported function of a module by name.
   *
   * @return The guest address, 0 when the module has no such export.
   */
  REX_KERNEL_API u32 GetExportAddress(std::string_view export_name) const;

  /** Returns the XEX image of a user module, null when the module is not an XEX. */
  REX_KERNEL_API rex::kernel::XEXModule* GetXEX();

  REX_KERNEL_API const rex::kernel::XEXModule* GetXEX() const;

  REX_KERNEL_API u32 GetTitleID() const override;

  REX_KERNEL_API std::vector<u32> GetAlternateTitleIDs() const override;

  const std::string& GetPath() const override { return path; }

  /**
   * Loads a module image from a file and applies a sibling XEX patch when one exists.
   *
   * @return X_STATUS_SUCCESS, or the status of the first step that failed.
   */
  REX_KERNEL_API X_STATUS LoadFromFile(TitleRuntime& owning_runtime, std::string_view guest_path);

  /**
   * Loads a module image from memory.
   *
   * @return X_STATUS_PENDING for an XEX that needs its second load step, X_STATUS_SUCCESS for an
   * ELF, or the failure status.
   */
  REX_KERNEL_API X_STATUS LoadFromMemory(TitleRuntime& owning_runtime, const void* addr,
                                         size_t length);

  /**
   * Unloads the image of a user module.
   *
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL when the image cannot be unloaded.
   */
  REX_KERNEL_API X_STATUS Unload();

  REX_KERNEL_API X_STATUS GetOptHeader(XEXHeaderKey key, void** out_ptr) override;

  /**
   * Finds an optional XEX header. For host use only.
   *
   * @tparam T The type of the value the header holds.
   * @param out_ptr Receives the header value or a pointer to its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  template <typename T>
  X_STATUS OptHeader(XEXHeaderKey key, T* out_ptr) {
    return GetOptHeader(key, reinterpret_cast<void**>(out_ptr));
  }

  /**
   * Finds an optional XEX header and returns an address that the title can read.
   *
   * @param out_header_guest_ptr Receives the header value or the guest address of its data.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_FOUND, or X_STATUS_UNSUCCESSFUL for an ELF.
   */
  REX_KERNEL_API X_STATUS GetOptHeader(XEXHeaderKey key, u32* out_header_guest_ptr) const;

  /**
   * Erases the cached thunks whose guest address falls in a range.
   *
   * @param hi The address after the last address of the range.
   */
  REX_KERNEL_API void InvalidateThunkCache(u32 lo, u32 hi);

  /** Binds a variable export to the guest address that holds it. */
  void SetVariable(u16 ordinal, u32 address) { kernel.variables.SetAddress(ordinal, address); }

  /** Returns the address of a variable export, or 0 when the module never created the variable. */
  u32 GetVariable(u16 ordinal) const { return kernel.variables.GetAddress(ordinal); }

  /**
   * Reads the handle that the loader data entry of an HMODULE holds.
   *
   * @param hmodule The host pointer of the loader data entry.
   */
  REX_KERNEL_API static u32 HandleFromHModule(void* hmodule);

  /**
   * Finds the module of an HMODULE.
   *
   * @param hmodule The host pointer of the loader data entry.
   * @return The module handle, 0 when the stored handle does not name a module.
   */
  REX_KERNEL_API static X_HANDLE FromHModule(TitleRuntime& runtime, void* hmodule);

  /**
   * Creates the `KernelObject` of a kernel module with its first handle.
   *
   * @param path The guest path of the module, its name is the file name of the path.
   * @return The handle with a count of 1, 0 when the table cannot grow.
   */
  REX_KERNEL_API static X_HANDLE CreateKernelModule(TitleRuntime& runtime, std::string_view path);

  /**
   * Creates the `KernelObject` of a user module with its first handle.
   *
   * @return The handle with a count of 1, 0 when the table cannot grow.
   */
  REX_KERNEL_API static X_HANDLE CreateUserModule(TitleRuntime& runtime);

  /**
   * Finds an optional header in an XEX header in memory and returns an address that the title can
   * read.
   *
   * @param header The XEX header.
   * @param out_header_guest_ptr Receives the header value or the guest address of its data.
   * @return X_STATUS_SUCCESS, or X_STATUS_NOT_FOUND.
   */
  REX_KERNEL_API static X_STATUS GetOptHeader(const kernel::Memory* memory, const XEXHeader* header,
                                              XEXHeaderKey key, u32* out_header_guest_ptr);
};

/**
 * Creates a `KernelObject` with its first handle and stores that handle in the loader data entry.
 *
 * @param type Whether the module is kernel or user.
 * @return The handle with a count of 1, 0 when the table cannot grow.
 */
REX_KERNEL_API X_HANDLE CreateModule(TitleRuntime& runtime, ModuleType type);

}  // namespace rex::kernel
