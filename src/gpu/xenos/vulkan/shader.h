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

#include <cstdint>

#include "gpu/xenos/pipeline/shader/spirv.h"
#include <rex/xenos/state.h>
#include "gpu/xenos/host/vulkan/vulkan_device.h"

namespace rex::gpu::vulkan {

class VulkanShader : public SPIRVShader {
 public:
  class VulkanTranslation : public SPIRVTranslation {
   public:
    explicit VulkanTranslation(VulkanShader& shader, u64 modification)
        : SPIRVTranslation(shader, modification) {}
    ~VulkanTranslation() override;

    VkShaderModule GetOrCreateShaderModule();
    VkShaderModule GetShaderModule() const { return shader_module_; }

   private:
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
  };

  explicit VulkanShader(const gpu::vulkan::VulkanDevice* vulkan_device,
                        xenos::ShaderType shader_type, u64 ucode_data_hash, const u32* ucode_dwords,
                        size_t ucode_dword_count,
                        std::endian ucode_source_endian = std::endian::big);

  size_t GetTextureBindingLayoutUserUID() const { return texture_binding_layout_user_uid_; }
  size_t GetSamplerBindingLayoutUserUID() const { return sampler_binding_layout_user_uid_; }
  /**
   * Reports whether the caller is the first to set up the binding layout identifiers.
   * Modifications of one shader can be translated on different threads.
   */
  bool EnterBindingLayoutUserUIDSetup() { return !binding_layout_user_uids_set_up_.test_and_set(); }
  void SetTextureBindingLayoutUserUID(size_t uid) { texture_binding_layout_user_uid_ = uid; }
  void SetSamplerBindingLayoutUserUID(size_t uid) { sampler_binding_layout_user_uid_ = uid; }

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(u64 modification) override;

 private:
  const gpu::vulkan::VulkanDevice* vulkan_device_;

  std::atomic_flag binding_layout_user_uids_set_up_;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
};

}  // namespace rex::gpu::vulkan
