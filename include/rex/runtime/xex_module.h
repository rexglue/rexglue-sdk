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

#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/runtime/module.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

constexpr memory::fourcc_t kXEX1Signature = memory::make_fourcc("XEX1");
constexpr memory::fourcc_t kXEX2Signature = memory::make_fourcc("XEX2");
constexpr memory::fourcc_t kElfSignature = memory::make_fourcc(0x7F, 'E', 'L', 'F');

class XexModule : public Module {
 public:
  struct ImportLibraryFn {
   public:
    uint32_t ordinal = 0;
    uint32_t value_address = 0;
    uint32_t thunk_address = 0;
  };
  struct ImportLibrary {
   public:
    std::string name;
    uint32_t id;
    xe_xex2_version_t version;
    xe_xex2_version_t min_version;
    std::vector<ImportLibraryFn> imports;
  };

  XexModule(FunctionDispatcher* function_dispatcher, Runtime& runtime);
  ~XexModule() override;

  bool loaded() const { return loaded_; }
  const filesystem::XEXImage& image() const { return image_; }
  const filesystem::PEInfo& pe() const { return pe_; }

  uint32_t image_size() const override;

  const std::vector<ImportLibrary>* import_libraries() const { return &import_libs_; }

  uint32_t base_address() const override { return base_address_; }

  uint32_t entry_point() const override {
    return image_.OptHeaderValue(XEX_HEADER_ENTRY_POINT).value_or(0);
  }

  uint32_t export_table_address() const override { return image_.security().export_table; }

  uint32_t exception_directory_rva() const override { return pe_.exception_directory_rva; }
  uint32_t exception_directory_size() const override { return pe_.exception_directory_size; }
  uint32_t exception_directory_address() const override {
    return base_address_ + pe_.exception_directory_rva;
  }

  std::span<const BinarySection> binary_sections() const override;
  const BinarySection* FindSectionByName(std::string_view name) const override;
  std::span<const BinarySymbol> binary_symbols() const override;

  uint32_t GetProcAddress(uint16_t ordinal) const;
  uint32_t GetProcAddress(const std::string_view name) const;

  int ApplyPatch(XexModule* module);
  bool Load(const std::string_view name, const std::string_view path, const void* xex_addr,
            size_t xex_length);
  bool LoadContinue();
  bool Unload();

  bool ContainsAddress(uint32_t address) override;

  const std::string& name() const override { return name_; }
  bool is_executable() const override {
    return (static_cast<u32>(image_.header().module_flags) & XEX_MODULE_TITLE) != 0;
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

  uint32_t base_address_ = 0;
  uint32_t low_address_ = 0;
  uint32_t high_address_ = 0;
};

}  // namespace rex::runtime
