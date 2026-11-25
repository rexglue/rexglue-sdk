// Fable II specific draw handling shared by the D3D12 and Vulkan backends.
//
// Each of these used to live in the D3D12 command processor only, so the
// Vulkan backend (the only one on Linux) missed them. Keeping one copy here
// means both backends decide the same way from the same cvars.

#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

REXCVAR_DECLARE(bool, terrain_tess_cpu);
REXCVAR_DECLARE(bool, skinning_cpu);
REXCVAR_DECLARE(bool, skip_depth_clear_transfers);
REXCVAR_DECLARE(std::string, depth_bias_shader);
REXCVAR_DECLARE(int32_t, depth_bias_shader_units);

namespace rex::graphics::fable2 {

// If the vertex shader is one of Fable II's memexport shaders that only read
// data the CPU wrote (terrain tessellation factors, skinning), computes its
// output into guest memory and returns true; its memexport result then
// doesn't have to be read back. The output range is stored in written_start /
// written_length when given. The CPU writes through the physical mapping,
// which doesn't fire the write watches: a caller that doesn't run the draw on
// the GPU must invalidate the GPU's copy of that range itself. Returns false,
// having written nothing, otherwise.
bool ComputeMemexportOnCpu(const RegisterFile& regs, memory::Memory& memory,
                           const Shader& vertex_shader, uint32_t* written_start = nullptr,
                           uint32_t* written_length = nullptr);

// Whether the draw is a Direct3D 9 style depth clear (a rectangle replacing
// all depth and stencil across the render target's width), so the old depth
// does not have to be transferred into the host render target first. Already
// checks skip_depth_clear_transfers and execute_unclipped_draw_vs_on_cpu, which
// bounds the transfer to the rectangle's height.
bool ShouldSkipDepthTransfers(const RegisterFile& regs, const memory::Memory& memory,
                              const Shader& vertex_shader);

// Extra host depth bias units for draws using the pixel shader named by
// depth_bias_shader (0 for every other shader). For geometry that relies on
// exact ties in the guest's 24-bit float depth, like the dog's shell fur.
int32_t GetShaderDepthBiasUnits(const Shader* pixel_shader);

}  // namespace rex::graphics::fable2
