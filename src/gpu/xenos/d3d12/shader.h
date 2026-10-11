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

#include "gpu/xenos/pipeline/shader/dxbc.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"

namespace rex::gpu::d3d12 {

class D3D12Shader : public DXBCShader {
 public:
  class D3D12Translation : public DXBCTranslation {
   public:
    D3D12Translation(D3D12Shader& shader, u64 modification)
        : DXBCTranslation(shader, modification) {}

    void DisassembleDXBCAndDxil(const gpu::d3d12::D3D12Provider& provider, bool disassemble_dxbc,
                                IDxbcConverter* dxbc_converter = nullptr,
                                IDxcUtils* dxc_utils = nullptr,
                                IDxcCompiler* dxc_compiler = nullptr);
  };

  D3D12Shader(xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
              size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);

  /**
   * Returns the unique identifier, used instead of a hash to rule out collisions, of the texture
   * binding layout used by the shader, for invalidation when a shader with an incompatible layout
   * is bound.
   *
   * @return The texture binding layout identifier.
   */
  size_t GetTextureBindingLayoutUserUID() const { return texture_binding_layout_user_uid_; }
  size_t GetSamplerBindingLayoutUserUID() const { return sampler_binding_layout_user_uid_; }
  /**
   * Claims the one time setup of the binding layout identifiers, as modifications of the same
   * shader can be translated on different threads. The setters require a true result.
   *
   * @return True when the caller must set the identifiers.
   */
  bool EnterBindingLayoutUserUIDSetup() { return !binding_layout_user_uids_set_up_.test_and_set(); }
  void SetTextureBindingLayoutUserUID(size_t uid) { texture_binding_layout_user_uid_ = uid; }
  void SetSamplerBindingLayoutUserUID(size_t uid) { sampler_binding_layout_user_uid_ = uid; }

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(u64 modification) override;

 private:
  std::atomic_flag binding_layout_user_uids_set_up_;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
};

}  // namespace rex::gpu::d3d12
