/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_DXBC_TO_DXIL_CONVERTER_H_
#define XENIA_GPU_METAL_DXBC_TO_DXIL_CONVERTER_H_

#include <cstdint>
#include <string>
#include <vector>

// Forward-declared so callers don't pull in the dxilconv/WinAdapter headers.
struct IDxbcConverter;

namespace rex {
namespace graphics {
namespace metal {

// Thin in-process wrapper around Microsoft's dxilconv library, which converts a
// DXBC container (produced by our DxbcShaderTranslator) into a DXIL container
// that Apple's Metal Shader Converter can consume. This is Stage 2 of the
// microcode -> DXBC -> DXIL -> metallib pipeline. No CLI process is spawned; the
// COM object is created in-process via DxcCreateInstance.
class DxbcToDxilConverter {
 public:
  DxbcToDxilConverter() = default;
  ~DxbcToDxilConverter() = default;

  // Verifies dxilconv can instantiate its converter. Must succeed before
  // Convert() is called. Safe to call once at startup.
  bool Initialize(std::string* error_out = nullptr);

  bool is_available() const { return is_available_; }

  // Converts a DXBC container to a DXIL container. Returns false and fills
  // error_out on failure. Thread-safe: each call creates (and releases) a
  // fresh converter instance — dxilconv converters carry call history that
  // corrupts later conversions, so instances are never reused or shared.
  bool Convert(const std::vector<uint8_t>& dxbc,
               std::vector<uint8_t>& dxil_out,
               std::string* error_out = nullptr);

 private:
  bool is_available_ = false;
  // Converter option string passed to Convert(); overridable via
  // XENIA_DXBC2DXIL_FLAGS for debugging. Stored as wide chars (the dxilconv ABI
  // takes LPCWSTR).
  std::wstring options_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_DXBC_TO_DXIL_CONVERTER_H_
