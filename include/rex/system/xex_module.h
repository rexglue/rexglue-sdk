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

#include <string>
#include <vector>

#include <rex/system/module.h>
#include <rex/system/util/xex2_info.h>

namespace rex::system {
class KernelState;
}  // namespace rex::system

namespace rex::runtime {

constexpr memory::fourcc_t kXEX1Signature = memory::make_fourcc("XEX1");
constexpr memory::fourcc_t kXEX2Signature = memory::make_fourcc("XEX2");
constexpr memory::fourcc_t kElfSignature = memory::make_fourcc(0x7F, 'E', 'L', 'F');

class Runtime;

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
  struct SecurityInfoContext {
    const char* rsa_signature;
    const char* aes_key;
    uint32_t image_size;
    uint32_t image_flags;
    uint32_t export_table;
    uint32_t load_address;
    uint32_t page_descriptor_count;
    const xex2_page_descriptor* page_descriptors;
  };
  enum XexFormat {
    kFormatUnknown,
    kFormatXex1,
    kFormatXex2,
  };

  XexModule(Processor* processor, system::KernelState* kernel_state);
  virtual ~XexModule();

  bool loaded() const { return loaded_; }
  const xex2_header* xex_header() const {
    return reinterpret_cast<const xex2_header*>(xex_header_mem_.data());
  }
  const SecurityInfoContext* xex_security_info() const { return &security_info_; }

  uint32_t image_size() const override {
    assert_not_zero(base_address_);

    // Calculate the new total size of the XEX image from its headers.
    auto heap = memory()->LookupHeap(base_address_);
    uint32_t total_size = 0;
    for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; i++) {
      // Byteswap the bitfield manually.
      xex2_page_descriptor desc;
      desc.value = rex::byte_swap(xex_security_info()->page_descriptors[i].value);

      total_size += desc.page_count * heap->page_size();
    }
    return total_size;
  }

  const std::vector<ImportLibrary>* import_libraries() const { return &import_libs_; }

  const xex2_opt_execution_info* opt_execution_info() const {
    xex2_opt_execution_info* retval = nullptr;
    GetOptHeader(XEX_HEADER_EXECUTION_INFO, &retval);
    return retval;
  }

  const xex2_opt_file_format_info* opt_file_format_info() const {
    xex2_opt_file_format_info* retval = nullptr;
    GetOptHeader(XEX_HEADER_FILE_FORMAT_INFO, &retval);
    return retval;
  }

  std::vector<uint32_t> opt_alternate_title_ids() const { return opt_alternate_title_ids_; }

  uint32_t base_address() const override { return base_address_; }
  bool is_dev_kit() const { return is_dev_kit_; }

  // Gets an optional header. Returns NULL if not found.
  // Special case: if key & 0xFF == 0x00, this function will return the value,
  // not a pointer! This assumes out_ptr points to uint32_t.
  static bool GetOptHeader(const xex2_header* header, xex2_header_keys key, void** out_ptr);
  bool GetOptHeader(xex2_header_keys key, void** out_ptr) const;

  // Ultra-cool templated version
  // Special case: if key & 0xFF == 0x00, this function will return the value,
  // not a pointer! This assumes out_ptr points to uint32_t.
  template <typename T>
  static bool GetOptHeader(const xex2_header* header, xex2_header_keys key, T* out_ptr) {
    return GetOptHeader(header, key, reinterpret_cast<void**>(out_ptr));
  }

  template <typename T>
  bool GetOptHeader(xex2_header_keys key, T* out_ptr) const {
    return GetOptHeader(key, reinterpret_cast<void**>(out_ptr));
  }

  static const void* GetSecurityInfo(const xex2_header* header);

  const PESection* GetPESection(const char* name);

  const std::vector<PESection>& pe_sections() const { return pe_sections_; }

  uint32_t entry_point() const override {
    uint32_t ep = 0;
    GetOptHeader(XEX_HEADER_ENTRY_POINT, &ep);
    return ep;
  }

  uint32_t export_table_address() const override { return xex_security_info()->export_table; }

  // Exception DataDirectory accessors (for PDATA table)
  // These return the correct PDATA location from the PE Optional Header,
  // which may differ from the .pdata section's VirtualAddress.
  uint32_t exception_directory_rva() const override { return exception_dir_rva_; }
  uint32_t exception_directory_size() const override { return exception_dir_size_; }
  uint32_t exception_directory_address() const override {
    return base_address_ + exception_dir_rva_;
  }

  // Binary introspection overrides
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
    return (xex_header()->module_flags & XEX_MODULE_TITLE) != 0;
  }

  bool is_valid_executable() const {
    assert_not_zero(base_address_);
    if (!base_address_) {
      return false;
    }
    uint8_t* buffer = memory()->TranslateVirtual(base_address_);
    return *(uint32_t*)buffer == 0x905A4D;
  }

  bool is_patch() const {
    assert_not_null(xex_header());
    if (!xex_header()) {
      return false;
    }
    return (xex_header()->module_flags &
            (XEX_MODULE_MODULE_PATCH | XEX_MODULE_PATCH_DELTA | XEX_MODULE_PATCH_FULL));
  }

 private:
  void ReadSecurityInfo();

  int ReadImage(const void* xex_addr, size_t xex_length, bool use_dev_key);
  int ReadImageUncompressed(const void* xex_addr, size_t xex_length);
  int ReadImageBasicCompressed(const void* xex_addr, size_t xex_length);
  int ReadImageCompressed(const void* xex_addr, size_t xex_length);

  int ReadPEHeaders();

  bool SetupLibraryImports(const std::string_view name, const xex2_import_library* library);

  void PopulateBinaryData();

  system::KernelState* kernel_state_ = nullptr;
  std::string name_;
  std::string path_;
  std::vector<uint8_t> xex_header_mem_;  // Holds the xex header
  std::vector<uint8_t> xexp_data_mem_;   // Holds XEXP patch data

  std::vector<ImportLibrary> import_libs_;  // pre-loaded import libraries for ease of use
  std::vector<PESection> pe_sections_;

  // XEX_HEADER_ALTERNATE_TITLE_IDS loaded into a safe std::vector
  std::vector<uint32_t> opt_alternate_title_ids_;

  uint8_t session_key_[0x10];
  bool is_dev_kit_ = false;

  bool loaded_ = false;         // Loaded into memory?
  bool finished_load_ = false;  // PE/imports/symbols/etc all loaded?

  uint32_t base_address_ = 0;
  uint32_t low_address_ = 0;
  uint32_t high_address_ = 0;

  // Exception DataDirectory from PE Optional Header
  uint32_t exception_dir_rva_ = 0;
  uint32_t exception_dir_size_ = 0;

  XexFormat xex_format_ = kFormatUnknown;
  SecurityInfoContext security_info_ = {};
};

}  // namespace rex::runtime
// (removed orphan xe namespace)
