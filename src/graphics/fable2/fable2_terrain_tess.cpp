#include "fable2_terrain_tess.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/types.h>

namespace rex::graphics::fable2 {

namespace {

constexpr uint32_t kOutputBytes = 16;
constexpr uint32_t kVertexBytes = 192;
constexpr uint32_t kMaxVertices = 4096;

// One vertex of the shader, generated from its disassembly.
template <typename ConstantFn, typename SampleFn>
void RunTerrainTessVS(float r[16][4], const ConstantFn& C, const SampleFn& sample) {
  bool p0 = false;
  float ps = 0.0f;
#include "fable2_terrain_tess_vs.inc"
}

float ReadBigEndianFloat(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  value = rex::byte_swap(value);
  float result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

}  // namespace

bool ComputeFable2TerrainTess(const RegisterFile& regs, memory::Memory& memory,
                              const Shader& vertex_shader, uint32_t& written_start, uint32_t& written_length) {
  const uint32_t constant_base = regs.Get<reg::SQ_VS_CONST>().base;
  const uint32_t* constant_registers = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
  const auto C = [&](uint32_t index, uint32_t component) {
    float value;
    std::memcpy(&value, &constant_registers[(constant_base + index) * 4 + component],
                sizeof(value));
    return value;
  };

  xenos::xe_gpu_memexport_stream_t stream = regs.GetMemExportStream(constant_base + 8);
  auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (stream.const_0x1 != 0x1 || stream.const_0x4b0 != 0x4B0 || stream.const_0x96 != 0x96 ||
      stream.format != xenos::ColorFormat::k_32_32_32_32_FLOAT ||
      stream.endianness != xenos::Endian128::k8in32 ||
      draw_initiator.source_select != xenos::SourceSelect::kAutoIndex ||
      vertex_shader.vertex_bindings().empty()) {
    return false;
  }
  xenos::xe_gpu_vertex_fetch_t vertex_fetch =
      regs.GetVertexFetch(vertex_shader.vertex_bindings()[0].fetch_constant);
  xenos::xe_gpu_texture_fetch_t texture_fetch = regs.GetTextureFetch(16);
  if (vertex_fetch.endian != xenos::Endian::k8in32 || texture_fetch.tiled ||
      (texture_fetch.format != xenos::TextureFormat::k_8 &&
       texture_fetch.format != xenos::TextureFormat::k_8_8_8_8)) {
    return false;
  }
  // The shader exports to element r0.x * c255.y, and r0.x includes the index offset.
  const uint32_t vertex_count = draw_initiator.num_indices;
  const uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  if (!vertex_count || vertex_count > kMaxVertices || C(255, 0) != 0.0f || C(255, 1) != 1.0f ||
      uint64_t(vertex_count) + index_offset > stream.index_count ||
      (uint64_t(vertex_count) + index_offset) * kVertexBytes > uint64_t(vertex_fetch.size) * 4) {
    return false;
  }
  const uint8_t* vertices = memory.TranslatePhysical<const uint8_t*>(vertex_fetch.address << 2);
  const uint8_t* texels =
      memory.TranslatePhysical<const uint8_t*>(uint32_t(texture_fetch.base_address) << 12);
  const uint32_t destination_address =
      uint32_t(stream.base_address << 2) + index_offset * kOutputBytes;
  uint8_t* destination = memory.TranslatePhysical<uint8_t*>(destination_address);
  if (!vertices || !texels || !destination) {
    return false;
  }

  const bool four_channels = texture_fetch.format == xenos::TextureFormat::k_8_8_8_8;
  const uint32_t bytes_per_texel = four_channels ? 4 : 1;
  const uint32_t row_bytes = (uint32_t(texture_fetch.pitch) << 5) * bytes_per_texel;
  const int32_t width = int32_t(texture_fetch.size_2d.width) + 1;
  const int32_t height = int32_t(texture_fetch.size_2d.height) + 1;
  const uint32_t swizzle = texture_fetch.swizzle;
  const auto sample = [&](float u, float v, uint32_t component) {
    const int32_t x = std::clamp(int32_t(std::floor(u)), 0, width - 1);
    const int32_t y = std::clamp(int32_t(std::floor(v)), 0, height - 1);
    const uint8_t* texel = texels + uint32_t(y) * row_bytes + uint32_t(x) * bytes_per_texel;
    const uint32_t select = (swizzle >> (component * 3)) & 7;
    if (select == 4) {
      return 0.0f;
    }
    if (select == 5) {
      return 1.0f;
    }
    uint32_t byte_value;
    if (four_channels) {
      // 32-bit big-endian texel, with red in the low byte.
      byte_value = texel[3 - select];
    } else {
      if (select != 0) {
        return select == 3 ? 1.0f : 0.0f;
      }
      byte_value = texel[0];
    }
    return float(byte_value) / 255.0f;
  };

  // The shader's 256 constants in a local array: read through the register
  // file they had to be loaded again after every store to a shader register,
  // as the compiler can't tell the two apart.
  float constants[256][4];
  std::memcpy(constants, &constant_registers[constant_base * 4], sizeof(constants));
  const auto LC = [&constants](uint32_t index, uint32_t component) {
    return constants[index][component];
  };

  static thread_local std::vector<uint8_t> output;
  output.resize(size_t(vertex_count) * kOutputBytes);
  for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
    const uint32_t index = vertex + index_offset;
    const uint8_t* attributes = vertices + size_t(index) * kVertexBytes;
    float r[16][4] = {};
    const auto fetch = [&](uint32_t reg, uint32_t attribute, uint32_t count, uint32_t first) {
      for (uint32_t i = 0; i < count; ++i) {
        r[reg][first + i] = ReadBigEndianFloat(attributes + attribute * 16 + i * 4);
      }
    };
    fetch(13, 0, 4, 0);
    fetch(12, 1, 4, 0);
    fetch(11, 2, 4, 0);
    fetch(10, 3, 4, 0);
    fetch(5, 4, 3, 0);
    fetch(3, 5, 3, 0);
    fetch(4, 6, 3, 0);
    fetch(7, 7, 3, 0);
    fetch(9, 8, 3, 0);
    fetch(8, 9, 3, 0);
    fetch(0, 10, 3, 1);
    fetch(6, 11, 3, 0);
    r[0][0] = float(index);
    RunTerrainTessVS(r, LC, sample);
    uint8_t* element = output.data() + size_t(vertex) * kOutputBytes;
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t value;
      std::memcpy(&value, &r[1][i], sizeof(value));
      value = rex::byte_swap(value);
      std::memcpy(element + i * 4, &value, sizeof(value));
    }
  }
  std::memcpy(destination, output.data(), output.size());
  written_start = destination_address;
  written_length = uint32_t(output.size());
  return true;
}

}  // namespace rex::graphics::fable2
