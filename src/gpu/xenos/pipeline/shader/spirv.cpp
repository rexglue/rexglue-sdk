/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include "gpu/xenos/pipeline/shader/spirv.h"

namespace rex::gpu {

SPIRVShader::SPIRVShader(xenos::ShaderType shader_type, u64 ucode_data_hash,
                         const u32* ucode_dwords, size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count, ucode_source_endian) {}

std::unique_ptr<Shader::Translation> SPIRVShader::CreateTranslationInstance(u64 modification) {
  return std::make_unique<SPIRVTranslation>(*this, modification);
}

}  // namespace rex::gpu
