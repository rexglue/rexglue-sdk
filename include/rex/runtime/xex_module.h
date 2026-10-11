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

#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/macros.h>
#include <rex/memory/fourcc.h>
#include <rex/runtime/module.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

constexpr memory::fourcc_t kXEX1Signature = memory::make_fourcc("XEX1");
constexpr memory::fourcc_t kXEX2Signature = memory::make_fourcc("XEX2");
constexpr memory::fourcc_t kElfSignature = memory::make_fourcc(0x7F, 'E', 'L', 'F');

/** A module loaded from an XEX image, or an XEX patch for 1. */
class REX_RUNTIME_API XEXModule : public Module {
 public:
  /** 1 import of an `ImportLibrary`. */
  struct ImportLibraryFn {
   public:
    u32 ordinal = 0;       /**< Export ordinal the import names. */
    u32 value_address = 0; /**< Guest address of the variable import slot, 0 when none. */
    u32 thunk_address = 0; /**< Guest address of the function import thunk, 0 when none. */
  };
  /** A library the image imports from. */
  struct ImportLibrary {
   public:
    std::string name;                     /**< Library name. */
    u32 id;                               /**< Library id in the image. */
    filesystem::XEXVersion version;       /**< Version the image was linked against. */
    filesystem::XEXVersion min_version;   /**< Oldest version the image accepts. */
    std::vector<ImportLibraryFn> imports; /**< Imports in ordinal order. */
  };

  /**
   * Builds an unloaded module.
   *
   * @param function_dispatcher The dispatcher whose memory the module loads into.
   * @param runtime The runtime the module belongs to.
   */
  XEXModule(FunctionDispatcher* function_dispatcher, Runtime& runtime);
  /** Destroys the module. */
  ~XEXModule() override;

  bool loaded() const { return loaded_; }
  const filesystem::XEXImage& image() const { return image_; }
  const filesystem::PEInfo& pe() const { return pe_; }

  /**
   * Returns the size of the loaded image.
   *
   * @return The size in bytes, the sum of the page ranges of the image.
   */
  u32 image_size() const override;

  u32 base_address() const override { return base_address_; }

  /**
   * Returns the entry point of the image.
   *
   * @return The guest address, 0 when the image has none.
   */
  u32 entry_point() const override {
    return image_.OptHeaderValue(filesystem::XEX_HEADER_ENTRY_POINT).value_or(0);
  }

  u32 export_table_address() const override { return image_.security().export_table; }

  u32 exception_directory_rva() const override { return pe_.exception_directory_rva; }
  u32 exception_directory_size() const override { return pe_.exception_directory_size; }
  /**
   * Returns the guest address of the exception directory.
   *
   * @return The guest address.
   */
  u32 exception_directory_address() const override {
    return base_address_ + pe_.exception_directory_rva;
  }

  /**
   * Returns the sections of the image.
   *
   * @return The sections.
   */
  std::span<const BinarySection> binary_sections() const override;
  /**
   * Returns the import symbols of the image.
   *
   * @return The symbols.
   */
  std::span<const BinarySymbol> binary_symbols() const override;

  /**
   * Looks up an export by ordinal.
   *
   * @param ordinal The export ordinal.
   * @return The guest address, 0 when the module has no such export.
   */
  u32 GetProcAddress(u16 ordinal) const;
  /**
   * Looks up an export by name.
   *
   * @param name The export name.
   * @return The guest address, 0 when the module has no such export.
   */
  u32 GetProcAddress(const std::string_view name) const;

  /**
   * Applies this patch image to a loaded module.
   *
   * @param module The module to patch.
   * @return 0 on success, 1 when this image is not a patch, another nonzero value on failure.
   */
  int ApplyPatch(XEXModule* module);
  /**
   * Parses and decodes an XEX image into guest memory.
   *
   * @param name The module name.
   * @param path The path the image came from.
   * @param xex_addr The host address of the XEX file.
   * @param xex_length The size of the file in bytes.
   * @return True on success.
   */
  bool Load(const std::string_view name, const std::string_view path, const void* xex_addr,
            size_t xex_length);
  /**
   * Parses the PE image and registers the imports and page protections once the image is decoded.
   *
   * @return True on success.
   */
  bool LoadContinue();
  /**
   * Releases the image memory.
   *
   * @return True.
   */
  bool Unload();

  const std::string& name() const override { return name_; }
  /**
   * Reports whether the image is a title module.
   *
   * @return True when the module flags mark a title.
   */
  bool is_executable() const override {
    return (static_cast<u32>(image_.header().module_flags) & filesystem::XEX_MODULE_TITLE) != 0;
  }

 private:
  bool SetupLibraryImports(std::string_view name, const filesystem::XEXImportLibrary& library);
  void PopulateBinaryData();

  Runtime* runtime_ = nullptr;
  std::string name_;
  std::string path_;
  filesystem::XEXImage image_;
  filesystem::PEInfo pe_;
  std::vector<ImportLibrary> import_libs_;

  bool loaded_ = false;
  bool finished_load_ = false;

  u32 base_address_ = 0;
};

}  // namespace rex::runtime
