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

#include <atomic>
#include <vector>

#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/state.h>

namespace rex::gpu {

class DXBCShader : public Shader {
 public:
  class DXBCTranslation : public Translation {
   public:
    DXBCTranslation(DXBCShader& shader, u64 modification) : Translation(shader, modification) {}
  };

  DXBCShader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
             size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);

  static constexpr u32 kMaxTextureBindingIndexBits =
      DXBCShaderTranslator::kMaxTextureBindingIndexBits;
  static constexpr u32 kMaxTextureBindings = DXBCShaderTranslator::kMaxTextureBindings;
  struct TextureBinding {
    u32 bindless_descriptor_index;
    u32 fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;
  };
  /** Returns the texture bindings, safe to hash and compare with memcmp. */
  const std::vector<TextureBinding>& GetTextureBindingsAfterTranslation() const {
    return translated_texture_bindings_;
  }
  u32 GetUsedTextureMaskAfterTranslation() const { return used_texture_mask_; }

  static constexpr u32 kMaxSamplerBindingIndexBits =
      DXBCShaderTranslator::kMaxSamplerBindingIndexBits;
  static constexpr u32 kMaxSamplerBindings = DXBCShaderTranslator::kMaxSamplerBindings;
  struct SamplerBinding {
    u32 bindless_descriptor_index;
    u32 fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;
  };
  const std::vector<SamplerBinding>& GetSamplerBindingsAfterTranslation() const {
    return sampler_bindings_;
  }

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(u64 modification) override;

 private:
  friend class DXBCShaderTranslator;

  std::atomic_flag bindings_setup_entered_;
  std::vector<TextureBinding> translated_texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  u32 used_texture_mask_ = 0;
};

}  // namespace rex::gpu
