#pragma once
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

#include <atomic>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

class SpirvShader : public Shader {
 public:
  class SpirvTranslation : public Translation {
   public:
    explicit SpirvTranslation(SpirvShader& shader, uint64_t modification)
        : Translation(shader, modification) {}
  };

  explicit SpirvShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian = std::endian::big);

  // Resource bindings are gathered after the successful translation of any
  // modification for simplicity of translation (and they don't depend on
  // modification bits).

  struct TextureBinding {
    uint32_t fetch_constant : 5;
    // Stacked and 3D are separate TextureBindings.
    xenos::FetchOpDimension dimension : 2;
    uint32_t is_signed : 1;
  };
  // Safe to hash and compare with memcmp for layout hashing.
  const std::vector<TextureBinding>& GetTextureBindingsAfterTranslation() const {
    return texture_bindings_;
  }
  const uint32_t GetUsedTextureMaskAfterTranslation() const { return used_texture_mask_; }

  struct SamplerBinding {
    uint32_t fetch_constant : 5;
    xenos::TextureFilter mag_filter : 2;
    xenos::TextureFilter min_filter : 2;
    xenos::TextureFilter mip_filter : 2;
    xenos::AnisoFilter aniso_filter : 3;
  };
  const std::vector<SamplerBinding>& GetSamplerBindingsAfterTranslation() const {
    return sampler_bindings_;
  }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  friend class SpirvShaderTranslator;

  std::atomic_flag bindings_setup_entered_ = ATOMIC_FLAG_INIT;
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  uint32_t used_texture_mask_ = 0;
};

}  // namespace rex::graphics
