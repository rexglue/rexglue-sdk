// Fable II runs its terrain tessellation in a vertex shader that memexports edge
// factors, which the game then needs in guest memory. Reading them back from the
// GPU costs a full GPU wait every frame, but the shader only depends on data the
// game writes from the CPU, so its output can be computed here instead.

#pragma once

#include <cstdint>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

constexpr uint64_t kFable2TerrainTessShader = 0x695413A9831D88DAull;

// Writes the draw's memexport stream to guest memory. Returns false, having
// written nothing, if the draw is not one this understands, so the GPU must run it.
// On success, written_start and written_length give the physical range written.
bool ComputeFable2TerrainTess(const RegisterFile& regs, memory::Memory& memory,
                              const Shader& vertex_shader, uint32_t& written_start,
                              uint32_t& written_length);

}  // namespace rex::graphics
