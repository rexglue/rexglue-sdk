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

#include <cstdint>
#include <string>

#include <rex/platform/dynlib.h>

#include <spirv-tools/libspirv.h>

namespace rex {
namespace ui {
namespace vulkan {

class SpirvToolsContext {
 public:
  SpirvToolsContext() {}
  SpirvToolsContext(const SpirvToolsContext& context) = delete;
  SpirvToolsContext& operator=(const SpirvToolsContext& context) = delete;
  ~SpirvToolsContext() { Shutdown(); }
  bool Initialize(unsigned int spirv_version);
  void Shutdown();

  spv_result_t Validate(const uint32_t* words, size_t num_words, std::string* error) const;

 private:
  rex::platform::DynamicLibrary library_;

  template <typename FunctionPointer>
  bool LoadLibraryFunction(FunctionPointer& function, const char* name) {
    function = library_.GetSymbol<FunctionPointer>(name);
    return function != nullptr;
  }
  decltype(&spvContextCreate) fn_spvContextCreate_ = nullptr;
  decltype(&spvContextDestroy) fn_spvContextDestroy_ = nullptr;
  decltype(&spvValidateBinary) fn_spvValidateBinary_ = nullptr;
  decltype(&spvDiagnosticDestroy) fn_spvDiagnosticDestroy_ = nullptr;

  spv_context context_ = nullptr;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
