#include "fable2_draw_hacks.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include "fable2_skinning.h"
#include "fable2_terrain_tess.h"

// These names are what the game project's fable2_config.toml [patches] section
// seeds, so they stay the same as when they were D3D12-only.
REXCVAR_DEFINE_BOOL(terrain_tess_cpu, true, "GPU",
                    "Compute Fable II's terrain tessellation factors on the CPU so the "
                    "memexport draw does not wait for the GPU every frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skinning_cpu, true, "GPU",
                    "Compute Fable II's memexport skinning on the CPU so the draw does not "
                    "wait for the GPU every frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skip_depth_clear_transfers, true, "GPU",
                    "Do not copy old depth into a render target for a rectangle that overwrites "
                    "all depth and stencil across its width, like a Direct3D 9 depth clear. "
                    "Only used with execute_unclipped_draw_vs_on_cpu, which limits the "
                    "transfer to the rectangle's height")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(depth_bias_shader, "", "GPU",
                      "Guest pixel shader ucode hash (hex) whose draws get "
                      "depth_bias_shader_units of extra host depth bias, for geometry that "
                      "relies on exact ties in the guest's 24-bit float depth (Z fighting on "
                      "the host's 32-bit depth). Host render target path only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(depth_bias_shader_units, 0, "GPU",
                     "Extra depth bias units (in the host API's constant depth bias units) for "
                     "depth_bias_shader.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, execute_unclipped_draw_vs_on_cpu);

namespace rex::graphics::fable2 {

bool ComputeMemexportOnCpu(const RegisterFile& regs, memory::Memory& memory,
                           const Shader& vertex_shader, uint32_t* written_start_out,
                           uint32_t* written_length_out) {
  uint32_t written_start = 0, written_length = 0;
  const uint64_t vs_hash = vertex_shader.ucode_data_hash();
  bool computed = false;
  if (vs_hash == kFable2TerrainTessShader) {
    computed = REXCVAR_GET(terrain_tess_cpu) &&
               ComputeFable2TerrainTess(regs, memory, vertex_shader, written_start,
                                        written_length);
  } else if (vs_hash == kFable2SkinningShader) {
    computed = REXCVAR_GET(skinning_cpu) &&
               ComputeFable2Skinning(regs, memory, vertex_shader, nullptr, written_start,
                                     written_length);
  }
  if (written_start_out) {
    *written_start_out = computed ? written_start : 0;
  }
  if (written_length_out) {
    *written_length_out = computed ? written_length : 0;
  }
  return computed;
}

namespace {

// A rectangle replacing all depth and stencil from the top left across the
// render target's width, whatever was there. Pitches are rounded up, so the
// rectangle may stop slightly short of the pitch.
bool IsDepthClearRectangle(const RegisterFile& regs, const memory::Memory& memory,
                           const Shader& vertex_shader) {
  constexpr float kPitchMargin = 80.0f;
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex || initiator.num_indices != 3 ||
      !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable || vertex_shader.vertex_bindings().empty()) {
    return false;
  }
  const reg::RB_DEPTHCONTROL depth = regs.Get<reg::RB_DEPTHCONTROL>();
  if (!depth.z_enable || !depth.z_write_enable || depth.zfunc != xenos::CompareFunction::kAlways ||
      !depth.stencil_enable || depth.stencilfunc != xenos::CompareFunction::kAlways ||
      depth.stencilzpass != xenos::StencilOp::kReplace ||
      regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask != 0xFF) {
    return false;
  }
  const Shader::VertexBinding& binding = vertex_shader.vertex_bindings()[0];
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
  const uint8_t* vertices = memory.TranslatePhysical<const uint8_t*>(fetch.address << 2);
  if (!vertices || fetch.endian != xenos::Endian::k8in32) {
    return false;
  }
  float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f;
  for (uint32_t i = 0; i < 3; ++i) {
    float position[2];
    for (uint32_t axis = 0; axis < 2; ++axis) {
      uint32_t raw;
      std::memcpy(&raw, vertices + i * binding.stride_words * 4 + axis * 4, sizeof(raw));
      raw = rex::byte_swap(raw);
      std::memcpy(&position[axis], &raw, sizeof(raw));
    }
    min_x = std::min(min_x, position[0]);
    max_x = std::max(max_x, position[0]);
    min_y = std::min(min_y, position[1]);
  }
  const float pitch = float(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch);
  return min_x <= 0.5f && min_y <= 0.5f && max_x >= pitch - 0.5f - kPitchMargin;
}

}  // namespace

bool ShouldSkipDepthTransfers(const RegisterFile& regs, const memory::Memory& memory,
                              const Shader& vertex_shader) {
  return REXCVAR_GET(skip_depth_clear_transfers) &&
         REXCVAR_GET(execute_unclipped_draw_vs_on_cpu) &&
         IsDepthClearRectangle(regs, memory, vertex_shader);
}

int32_t GetShaderDepthBiasUnits(const Shader* pixel_shader) {
  if (!pixel_shader) {
    return 0;
  }
  const std::string& bias_shader = REXCVAR_GET(depth_bias_shader);
  if (bias_shader.empty() ||
      std::strtoull(bias_shader.c_str(), nullptr, 16) != pixel_shader->ucode_data_hash()) {
    return 0;
  }
  return REXCVAR_GET(depth_bias_shader_units);
}

}  // namespace rex::graphics::fable2
