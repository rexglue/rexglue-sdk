/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Static Recompilation Runtime
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 *
 * DXBC to DXIL converter wrapper for the Metal shader pipeline.
 *
 * Converts SM5.1 DXBC bytecode (produced by the Xenos ucode → DXBC translator)
 * into SM6.0 DXIL bytecode, which can then be fed to Apple's Metal Shader
 * Converter (MSC) to produce Metal IR / metallib data.
 *
 * Pipeline: Xenos ucode → DXBC → [this] → DXIL → MSC → Metal IR → MTLLibrary
 *
 * Uses Microsoft's dxilconv library (IDxbcConverter) loaded dynamically.
 * On macOS this requires a cross-compiled or ported dxilconv dylib.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declare the official IDxbcConverter interface
struct IDxbcConverter;

namespace rex::graphics::metal {

class DxbcToDxilConverter {
 public:
  DxbcToDxilConverter();
  ~DxbcToDxilConverter();
  DxbcToDxilConverter(const DxbcToDxilConverter&) = delete;
  DxbcToDxilConverter& operator=(const DxbcToDxilConverter&) = delete;

  // Initialize the converter (locate and load dxilconv library).
  // Returns false if the library is not available.
  bool Initialize();

  // Convert DXBC bytecode to DXIL bytecode.
  // Returns true on success, populating dxil_data_out.
  // On failure, returns false and optionally sets error_message.
  bool Convert(const std::vector<uint8_t>& dxbc_data,
               std::vector<uint8_t>& dxil_data_out,
               std::string* error_message = nullptr);

  // Check if the converter is available and initialized.
  bool IsAvailable() const { return is_available_; }

 private:
  bool is_available_ = false;

  // Opaque handle to the dynamically loaded dxilconv library.
  void* dxilconv_lib_ = nullptr;

  // Function pointer types for dxilconv API.
  using DxcCreateInstanceFn = int32_t (*)(const void* clsid, const void* iid,
                                          void** instance);
  DxcCreateInstanceFn create_instance_fn_ = nullptr;

  // Per-thread converter instance management.
  IDxbcConverter* GetThreadConverter(std::string* error_message);

  // Debug: write blob to file.
  bool WriteDebugFile(const std::string& path,
                      const std::vector<uint8_t>& data);
};

}  // namespace rex::graphics::metal
