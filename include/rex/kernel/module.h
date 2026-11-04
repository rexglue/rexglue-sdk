/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/module.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <optional>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/symbol_map.h>
#include <memory>

#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/memory/endian.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/platform/lock_order.h>
#include <rex/result.h>
#include <rex/kernel/types/status.h>
#include <rex/module.h>
#include <rex/utility.h>

namespace rex::kernel {

class FunctionDispatcher;

class REX_KERNEL_API GuestModule {
 public:
  /**
   * Builds a module.
   *
   * @param function_dispatcher The dispatcher that supplies the memory, may be null.
   */
  explicit GuestModule(FunctionDispatcher* function_dispatcher);
  virtual ~GuestModule();

  kernel::GuestAddressSpace* GetAddressSpace() const { return memory_; }
  FunctionDispatcher* GetFunctionDispatcher() const { return function_dispatcher_; }

  virtual const std::string& GetName() const = 0;
  virtual bool IsExecutable() const = 0;

  /**
   * @return The guest address, 0 by default.
   */
  virtual u32 GetBaseAddress() const { return 0; }
  /**
   * @return The size in bytes, 0 by default.
   */
  virtual u32 GetImageSize() const { return 0; }
  /**
   * @return The guest address, 0 by default.
   */
  virtual u32 GetEntryPoint() const { return 0; }
  /**
   * @return The guest address, 0 by default.
   */
  virtual u32 GetExportTableAddress() const { return 0; }

  /** Returns the RVA of the PDATA table in the exception data directory, 0 by default. */
  virtual u32 GetExceptionDirectoryRVA() const { return 0; }
  /** Returns the size of the PDATA table in the exception data directory, 0 by default. */
  virtual u32 GetExceptionDirectorySize() const { return 0; }
  /** Returns the guest address of the PDATA table in the exception data directory, 0 by default. */
  virtual u32 GetExceptionDirectoryAddress() const { return 0; }

  bool IsExecutableAddress(u32 address) const;

  /**
   * Returns the binary sections, empty by default.
   */
  virtual std::span<const BinarySection> GetBinarySections() const;
  /**
   * @return The section, or null when none contains the address.
   */
  virtual const BinarySection* FindSectionByAddress(u32 address) const;

  /**
   * Returns the binary symbols, empty by default.
   */
  virtual std::span<const BinarySymbol> GetBinarySymbols() const;

 protected:
  FunctionDispatcher* function_dispatcher_ = nullptr;
  kernel::GuestAddressSpace* memory_ = nullptr;

  /** Populated by derived classes. */
  std::vector<BinarySection> binary_sections_;
  std::vector<BinarySymbol> binary_symbols_;
};

}  // namespace rex::kernel

namespace rex::kernel {

/** Loads libxenon executables. */
class REX_KERNEL_API ELFModule : public GuestModule {
 public:
  /**
   * Builds an unloaded module.
   *
   * @param function_dispatcher The dispatcher whose memory the module loads into.
   */
  explicit ELFModule(FunctionDispatcher* function_dispatcher);
  virtual ~ELFModule();

  bool IsLoaded() const { return loaded_; }
  const std::string& GetName() const override { return name_; }
  /** Reports whether the image has an entry point. */
  bool IsExecutable() const override;
  const std::string& GetPath() const { return path_; }

  /** Returns the lowest guest address of the loaded segments, 0 before the image is loaded. */
  u32 GetBaseAddress() const override { return base_address_; }
  u32 GetImageSize() const override { return image_size_; }
  u32 GetEntryPoint() const override { return entry_point_; }

  /**
   * Copies the loadable segments of an ELF image into guest memory.
   *
   * @param elf_addr The host address of the ELF image.
   * @return True on success, false when the data is not an ELF or has no entry point.
   */
  bool Load(const std::string_view name, const std::string_view path, const void* elf_addr,
            size_t elf_length);
  bool Unload();

 private:
  std::string name_;
  std::string path_;

  bool loaded_ = false;
  std::vector<u8> elf_header_mem_;
  u32 entry_point_ = 0;
  u32 base_address_ = 0;
  u32 image_size_ = 0;
};

}  // namespace rex::kernel

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

constexpr memory::FourCCT kXEX1Signature = memory::make_fourcc("XEX1");
constexpr memory::FourCCT kXEX2Signature = memory::make_fourcc("XEX2");
constexpr memory::FourCCT kELFSignature = memory::make_fourcc(0x7F, 'E', 'L', 'F');

/** A module loaded from an XEX image, or from an XEX patch image. */
class REX_KERNEL_API XEXModule : public GuestModule {
 public:
  struct LibraryImport {
   public:
    u32 ordinal = 0;
    u32 value_address = 0; /**< Guest address of the variable import slot, 0 when none. */
    u32 thunk_address = 0; /**< Guest address of the function import thunk, 0 when none. */
  };
  struct ImportLibrary {
   public:
    std::string name;      /**< Library name without directory and extension. */
    std::string file_name; /**< Library name as the image writes it. */
    u32 id;
    XEXVersion version;                 /**< Version the image was linked against. */
    XEXVersion min_version;             /**< Oldest version the image accepts. */
    std::vector<LibraryImport> imports; /**< Imports in ordinal order. */
  };

  /**
   * Builds an unloaded module.
   *
   * @param function_dispatcher The dispatcher whose memory the module loads into.
   */
  XEXModule(FunctionDispatcher* function_dispatcher, TitleRuntime& runtime);
  ~XEXModule() override;

  bool IsLoaded() const { return loaded_; }
  const filesystem::IXEXImage& GetImage() const { return *image_; }
  const filesystem::PEInfo& GetPE() const { return pe_; }

  /**
   * @return Size in bytes, summed over the page ranges of the image.
   */
  u32 GetImageSize() const override;

  /**
   * @return The guest address, 0 before the image is loaded.
   */
  u32 GetBaseAddress() const override { return base_address_; }

  /**
   * @return The guest address, 0 when the image has none.
   */
  u32 GetEntryPoint() const override {
    return image_ ? image_->GetOptHeaderValue(kXEXHeaderEntryPoint).value_or(0) : 0;
  }

  /**
   * @return The guest address, 0 when the image has none.
   */
  u32 GetExportTableAddress() const override {
    return image_ ? image_->GetSecurity().export_table : 0;
  }

  u32 GetExceptionDirectoryRVA() const override { return pe_.exception_directory_rva; }
  u32 GetExceptionDirectorySize() const override { return pe_.exception_directory_size; }
  u32 GetExceptionDirectoryAddress() const override {
    return base_address_ + pe_.exception_directory_rva;
  }

  std::span<const BinarySection> GetBinarySections() const override;
  /**
   * Returns the import symbols of the image.
   */
  std::span<const BinarySymbol> GetBinarySymbols() const override;

  /**
   * Looks up an export by ordinal.
   *
   * @return The guest address, 0 when the module has no such export.
   */
  u32 GetExportAddress(u16 ordinal) const;

  const std::vector<ImportLibrary>& GetImportLibraries() const { return import_libs_; }
  /**
   * Looks up an export by name.
   *
   * @return The guest address, 0 when the module has no such export.
   */
  u32 GetExportAddress(const std::string_view name) const;

  /**
   * Applies this patch image to a loaded module.
   *
   * @return 0 on success, 1 when this image is not a patch, another nonzero value on failure.
   */
  int ApplyPatch(XEXModule* module);
  /**
   * Parses and decodes an XEX image into host memory and reserves its guest memory.
   *
   * @param xex_addr The host address of the XEX file.
   */
  bool Load(const std::string_view name, const std::string_view path, const void* xex_addr,
            size_t xex_length);
  /**
   * Returns the decoded image that waits in host memory between Load and LoadContinue, empty once
   * LoadContinue has copied the image into guest memory.
   */
  std::span<const u8> GetPendingImage() const { return pending_image_; }
  /**
   * Parses the PE image, copies it into guest memory without its header, then registers the
   * imports and page protections.
   */
  bool LoadContinue();
  bool Unload();

  const std::string& GetName() const override { return name_; }
  /** Reports whether the module flags mark a title module. */
  bool IsExecutable() const override {
    return image_ && (static_cast<u32>(image_->GetHeader().module_flags) & kXEXModuleTitle) != 0;
  }

 private:
  bool SetupLibraryImports(std::string_view name, const filesystem::XEXImportLibrary& library);
  void PopulateBinaryData();

  TitleRuntime* runtime_ = nullptr;
  std::string name_;
  std::string path_;
  std::unique_ptr<filesystem::IXEXImage> image_;
  filesystem::PEInfo pe_;
  std::vector<u8> pending_image_;
  std::vector<ImportLibrary> import_libs_;

  bool loaded_ = false;
  bool finished_load_ = false;

  u32 base_address_ = 0;
};

}  // namespace rex::kernel

namespace rex::kernel {

/** A function another loaded module exports by ordinal, resolved on its first call. */
struct ModuleImport {
  /**
   * @param library_name The base name of the exporting module, without directory and extension.
   */
  constexpr ModuleImport(const char* library_name, u16 export_ordinal)
      : library(library_name), ordinal(export_ordinal) {}

  const char* library; /**< Base name of the exporting module. */
  u16 ordinal;
  std::atomic<PPCFunc*> function{nullptr};
  std::atomic<u64> generation{0}; /**< Registry generation the function was resolved at. */
};

/**
 * Resolves an import to the function of the exporting module, and resolves it again after a
 * module loads or unloads. Stops the title when the module or the export is gone.
 */
REX_KERNEL_API PPCFunc* ResolveModuleImport(ModuleImport& target);

}  // namespace rex::kernel

namespace rex::kernel {

/**
 * Lists, loads and unloads modules. The caller must not close a handle it returns.
 */
REX_DECLARE_INTERFACE(IModuleRegistry, 1)
  virtual ~IModuleRegistry() = default;

  /**
   * Finds a loaded module.
   *
   * @param name A module name, a file name or a path, empty for the executable.
   * @param user_only True to skip the kernel modules.
   * @return The handle, empty when no module matches or the name is kernel32.dll.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> GetModule(std::string_view name,
                                                          bool user_only = false) = 0;

  /**
   * @return The handle, empty before an executable is loaded.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> GetExecutable() const = 0;

  /**
   * @param name A module name, a file name or a path.
   * @return The handle, empty when no kernel module matches.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> FindKernelModule(std::string_view name) = 0;

  /**
   * @param name A module name, a file name or a path.
   * @return The handle, empty when no user module matches.
   */
  [[nodiscard]] virtual std::optional<X_HANDLE> FindUserModule(std::string_view name) = 0;

  /**
   * @param name A module name, a file name or a path.
   * @return True when a kernel module matches, false for an empty name.
   */
  [[nodiscard]] virtual bool IsKernelModule(std::string_view name) = 0;

  /** Lists the kernel modules in load order. */
  [[nodiscard]] virtual std::vector<X_HANDLE> GetKernelModules() = 0;

  /** Lists the loaded user modules in load order. */
  [[nodiscard]] virtual std::vector<X_HANDLE> GetUserModules() = 0;

  /**
   * Loads a user module or finds the loaded one. The caller holds 1 load reference, released by
   * XexUnloadImage.
   *
   * @param name A path, or a file name relative to the executable.
   * @param call_entry True to call the entry point of a DLL module for the process attach.
   * @return The handle, or the load status. X_STATUS_UNSUCCESSFUL when another thread loads the
   *         same path, the recompiled library does not match, or the entry point refused.
   */
  [[nodiscard]] virtual Result<X_HANDLE> LoadUserModule(std::string_view name,
                                                        bool call_entry = true) = 0;

  /**
   * Unloads a user module, releases its recompiled library at teardown and closes its handles.
   *
   * @param call_entry True to call the entry point of a DLL module for the process detach.
   */
  virtual void UnloadUserModule(X_HANDLE module, bool call_entry = true) = 0;

  /**
   * Adds a module path tried when the importer directory has no match. An import name matches its
   * file name ignoring case.
   */
  virtual void AddImportCandidate(std::string_view path) = 0;

  /**
   * Registers a kernel module.
   *
   * @param created The handle a kernel module creator returned.
   * @return The handle the registry owns, or X_STATUS_INVALID_HANDLE.
   */
  [[nodiscard]] virtual Result<X_HANDLE> RegisterKernelModule(X_HANDLE created) = 0;

  /**
   * Registers the recompiled library that serves a title module path.
   *
   * @param pe_name The PE name of the module.
   * @param shared_lib_name The host library, empty when the code is linked into the executable.
   */
  virtual void RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                        const char* shared_lib_name) = 0;

  /** Returns the lock that serializes module load count changes against module lookups. */
  [[nodiscard]] virtual rex::platform::RankedMutex& GetLoadCountMutex() = 0;
};

}  // namespace rex::kernel
