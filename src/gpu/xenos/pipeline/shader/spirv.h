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

#pragma once

#include <atomic>
#include <vector>

#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/xenos/xenos.h>

namespace rex::gpu {

class SPIRVShader : public Shader {
 public:
  class SPIRVTranslation : public Translation {
   public:
    explicit SPIRVTranslation(SPIRVShader& shader, u64 modification)
        : Translation(shader, modification) {}
  };

  explicit SPIRVShader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
                       size_t ucode_dword_count,
                       std::endian ucode_source_endian = std::endian::big);

  struct TextureBinding {
    u32 fetch_constant : 5;
    xenos::FetchOpDimension dimension : 2;
    u32 is_signed : 1;
  };
  // Safe to hash and compare with memcmp for layout hashing.
  const std::vector<TextureBinding>& GetTextureBindingsAfterTranslation() const {
    return texture_bindings_;
  }
  u32 GetUsedTextureMaskAfterTranslation() const { return used_texture_mask_; }

  struct SamplerBinding {
    u32 fetch_constant : 5;
    xenos::TextureFilter mag_filter : 2;
    xenos::TextureFilter min_filter : 2;
    xenos::TextureFilter mip_filter : 2;
    xenos::AnisoFilter aniso_filter : 3;
  };
  const std::vector<SamplerBinding>& GetSamplerBindingsAfterTranslation() const {
    return sampler_bindings_;
  }

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(u64 modification) override;

 private:
  friend class SPIRVShaderTranslator;

  std::atomic_flag bindings_setup_entered_;
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  u32 used_texture_mask_ = 0;
};

}  // namespace rex::gpu
