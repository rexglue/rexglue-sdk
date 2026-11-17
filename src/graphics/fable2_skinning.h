// Fable II skins some vertices in a vertex shader that memexports them to guest
// memory. Reading them back from the GPU costs a full GPU wait every frame, but
// the shader only depends on data the game writes from the CPU, so its output can
// be computed here instead.

#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

constexpr uint64_t kFable2SkinningShader = 0xF6C546D6C2645541ull;

// Computes the draw's memexport output, into guest memory or, if capture is not
// null, into capture. On writing guest memory, written_start and written_length give
// the physical range. Returns false, having written nothing, if the draw is not
// one this understands, so the GPU must run it.
bool ComputeFable2Skinning(const RegisterFile& regs, memory::Memory& memory,
                           const Shader& vertex_shader, std::vector<uint8_t>* capture,
                           uint32_t& written_start, uint32_t& written_length);

}  // namespace rex::graphics
