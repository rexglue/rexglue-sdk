/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_H_
#define XENIA_GPU_METAL_METAL_SHADER_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "xxhash.h"
#include "gpu/xenos/pipeline/shader/dxbc.h"
#include "rex/graphics/metal/metal_fetch_static_info.h"  // upstream DXBCShader
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// A guest shader on the Metal backend. Reuses the DXBC front half (the Xbox-360
// microcode -> DXBC translator lives in DxbcShader), then each translation adds
// the DXIL -> metallib -> MTLFunction back half via the Metal Shader Converter
// chain. Mirrors D3D12Shader/DxbcShader; the ObjC Metal objects are opaque
// pointers so this header stays plain C++.
class MetalShader : public ::rex::gpu::DXBCShader {
 public:
  // The port's own fetch-pattern analysis, shared with the command processor.
  using FetchStaticInfo = MetalFetchStaticInfo;

  // Index of this shader's ucode in the pipeline cache's storage; the port's own bookkeeping, used to notice a shader whose
  // ucode was replaced after it was first translated.
  uint32_t ucode_storage_index() const { return ucode_storage_index_; }
  void set_ucode_storage_index(uint32_t index) { ucode_storage_index_ = index; }


 public:
  class MetalTranslation : public ::rex::gpu::DXBCShader::DXBCTranslation {
   public:
    MetalTranslation(MetalShader& shader, uint64_t modification)
        : ::rex::gpu::DXBCShader::DXBCTranslation(shader, modification) {}
    ~MetalTranslation();


    // id<MTLLibrary> / id<MTLFunction>, retained; set once the metallib has been
    // produced and loaded onto the device.
    void* metal_library() const { return metal_library_; }
    void* metal_function() const { return metal_function_; }
    bool has_metal_function() const { return metal_function_ != nullptr; }

    // Builds the MTLLibrary + entry MTLFunction from a metallib blob on the given
    // id<MTLDevice>. Returns false on failure. Takes ownership of nothing; the
    // caller supplies the metallib bytes.
    bool CreateMetalFunction(void* device, const std::vector<uint8_t>& metallib);

    // True once MetalPipelineCache::TranslateShader has run every stage for
    // this translation, whatever the outcome. is_translated() turns true at
    // the end of the DXBC stage, while DXBC->DXIL->metallib may still be
    // running on another thread, so code that reads the result without the
    // shader's translate mutex must check this instead.
    // Index of this shader's ucode in the pipeline cache's storage; the port's own bookkeeping, used to detect a shader whose
    // ucode was replaced after it was first translated.
    uint32_t ucode_storage_index() const { return ucode_storage_index_; }
    void set_ucode_storage_index(uint32_t index) { ucode_storage_index_ = index; }

    bool is_metal_translated() const {
      return metal_translated_.load(std::memory_order_acquire);
    }
    void MarkMetalTranslated() {
      metal_translated_.store(true, std::memory_order_release);
    }

   private:
    uint32_t ucode_storage_index_ = 0;

    void* metal_library_ = nullptr;   // id<MTLLibrary> (retained)
    void* metal_function_ = nullptr;  // id<MTLFunction> (retained)
    std::atomic<bool> metal_translated_{false};
  };

  // Held for the whole of MetalPipelineCache::TranslateShader of any of this
  // shader's translations, so the GPU thread and a background pipeline worker
  // never translate it at once (the first translation also fills the
  // shader's texture and sampler bindings).
  std::mutex& translate_mutex() { return translate_mutex_; }

  MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords, size_t ucode_dword_count,
              std::endian ucode_source_endian = std::endian::big);

 public:
  // Fetch-plain patterns (see ::rex::gpu::Shader::GetFetchPlainPattern). Returns the
  // 1-based index of `mask`, adding it while the table has room (15), else
  // 0 (full decode). Patterns are stable per material, so a shader sees few.
  // Fetch-plain patterns (see ::rex::gpu::Shader::GetFetchStaticInfo): per pattern, the
  // static record of every fetch constant the shader samples (others zero).
  struct FetchPattern {
    uint32_t fetch_mask;
    FetchStaticInfo info[32];
    bool operator==(const FetchPattern& other) const {
      return fetch_mask == other.fetch_mask &&
             std::memcmp(info, other.info, sizeof(info)) == 0;
    }
  };
  // Returns the 1-based index of `pattern`, adding it while the table has
  // room (15), else 0 (fully dynamic translation). Patterns are stable per
  // material, so a shader sees few.
  uint32_t FindOrAddFetchPattern(const FetchPattern& pattern) {
    for (size_t i = 0; i < fetch_patterns_.size(); ++i) {
      if (fetch_patterns_[i] == pattern) {
        return uint32_t(i + 1);
      }
    }
    if (fetch_patterns_.size() >= 15) {
      return 0;
    }
    // Never reallocates: translations read entries from other threads.
    fetch_patterns_.reserve(15);
    fetch_patterns_.push_back(pattern);
    return uint32_t(fetch_patterns_.size());
  }
  // The pattern behind a 1-based index, or null.
  const FetchPattern* GetFetchPattern(uint32_t pattern_index) const {
    if (!pattern_index || pattern_index > fetch_patterns_.size()) {
      return nullptr;
    }
    return &fetch_patterns_[pattern_index - 1];
  }
  // Content hash of a pattern (0 for none). The index is session-local
  // (discovery order), so the durable metallib key carries this instead:
  // keyed on the index, a warm cache served shaders compiled with another
  // session's sign / scale / bias literals.
  uint64_t FetchPatternContentHash(uint32_t pattern_index) const {
    if (!pattern_index || pattern_index > fetch_patterns_.size()) {
      return 0;
    }
    const FetchPattern& p = fetch_patterns_[pattern_index - 1];
    struct Entry {
      uint32_t slot;
      uint32_t integer_scale_bits;
      uint32_t swizzled_signs;
      int32_t exp_adjust;
    };
    Entry entries[32];
    uint32_t count = 0;
    for (uint32_t i = 0; i < 32; ++i) {
      if (p.fetch_mask & (1u << i)) {
        entries[count++] = {i, p.info[i].integer_scale_bits,
                            p.info[i].swizzled_signs, p.info[i].exp_adjust};
      }
    }
    return XXH3_64bits(entries, sizeof(Entry) * count) | 1;
  }
  uint32_t GetFetchPlainPattern(uint32_t pattern_index) const {
    if (!pattern_index || pattern_index > fetch_patterns_.size()) {
      return 0;
    }
    const FetchPattern& p = fetch_patterns_[pattern_index - 1];
    uint32_t plain = 0;
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(p.fetch_mask & (1u << i))) {
        continue;
      }
      const uint8_t signs = p.info[i].swizzled_signs;
      bool ok = true;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sign = (signs >> (c * 2)) & 3;
        if (sign == uint32_t(xenos::TextureSign::kUnsignedBiased) ||
            sign == uint32_t(xenos::TextureSign::kGamma)) {
          ok = false;
        }
      }
      if (ok) {
        plain |= 1u << i;
      }
    }
    return plain;
  }
  bool GetFetchStaticInfo(uint32_t pattern_index, uint32_t tfetch_index,
                          FetchStaticInfo* out) const {
    if (!pattern_index || pattern_index > fetch_patterns_.size() ||
        tfetch_index >= 32) {
      return false;
    }
    const FetchPattern& p = fetch_patterns_[pattern_index - 1];
    if (!(p.fetch_mask & (1u << tfetch_index))) {
      return false;
    }
    *out = p.info[tfetch_index];
    return true;
  }

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(uint64_t modification) override;

 private:
  std::mutex translate_mutex_;
  std::vector<FetchPattern> fetch_patterns_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SHADER_H_
