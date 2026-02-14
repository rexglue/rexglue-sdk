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

#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/logging.h>
#include <rex/ui/vulkan/spirv_tools_context.h>

namespace rex {
namespace ui {
namespace vulkan {

bool SpirvToolsContext::Initialize(unsigned int spirv_version) {
  const char* vulkan_sdk_env = std::getenv("VULKAN_SDK");
  if (!vulkan_sdk_env) {
    REXLOG_ERROR("SPIRV-Tools: Failed to get the VULKAN_SDK environment variable");
    Shutdown();
    return false;
  }
  std::filesystem::path vulkan_sdk_path(vulkan_sdk_env);
  auto library_path = vulkan_sdk_path / platform::lib_names::kSpirvToolsSdkPath;
  if (!library_.Load(library_path)) {
    REXLOG_ERROR("SPIRV-Tools: Failed to load {}", library_path.string());
    Shutdown();
    return false;
  }
  if (!LoadLibraryFunction(fn_spvContextCreate_, "spvContextCreate") ||
      !LoadLibraryFunction(fn_spvContextDestroy_, "spvContextDestroy") ||
      !LoadLibraryFunction(fn_spvValidateBinary_, "spvValidateBinary") ||
      !LoadLibraryFunction(fn_spvDiagnosticDestroy_, "spvDiagnosticDestroy")) {
    REXLOG_ERROR("SPIRV-Tools: Failed to get library function pointers");
    Shutdown();
    return false;
  }
  spv_target_env target_env;
  if (spirv_version >= 0x10500) {
    target_env = SPV_ENV_VULKAN_1_2;
  } else if (spirv_version >= 0x10400) {
    target_env = SPV_ENV_VULKAN_1_1_SPIRV_1_4;
  } else if (spirv_version >= 0x10300) {
    target_env = SPV_ENV_VULKAN_1_1;
  } else {
    target_env = SPV_ENV_VULKAN_1_0;
  }
  context_ = fn_spvContextCreate_(target_env);
  if (!context_) {
    REXLOG_ERROR("SPIRV-Tools: Failed to create a Vulkan 1.0 context");
    Shutdown();
    return false;
  }
  return true;
}

void SpirvToolsContext::Shutdown() {
  if (context_) {
    fn_spvContextDestroy_(context_);
    context_ = nullptr;
  }
  library_.Close();
}

spv_result_t SpirvToolsContext::Validate(const uint32_t* words, size_t num_words,
                                         std::string* error) const {
  if (error) {
    error->clear();
  }
  if (!context_) {
    return SPV_UNSUPPORTED;
  }
  spv_diagnostic diagnostic = nullptr;
  spv_result_t result = fn_spvValidateBinary_(context_, words, num_words, &diagnostic);
  if (diagnostic) {
    if (error && diagnostic && diagnostic->error) {
      *error = diagnostic->error;
    }
    fn_spvDiagnosticDestroy_(diagnostic);
  }
  return result;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
