#include "fable2_skinning.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/types.h>

namespace rex::graphics::fable2 {

namespace {

constexpr uint32_t kOutputDwords = 5;
constexpr uint32_t kVertexDwords = 7;
constexpr uint32_t kBoneDwords = 6;
constexpr uint32_t kMaxVertices = 1u << 20;

uint32_t ReadBigEndian(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return rex::byte_swap(value);
}

void WriteBigEndian(uint8_t* bytes, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(bytes, &value, sizeof(value));
}

float HalfToFloat(uint32_t half) {
  const uint32_t sign = (half & 0x8000) << 16;
  const uint32_t exponent = (half >> 10) & 0x1F;
  const uint32_t mantissa = half & 0x3FF;
  uint32_t bits;
  if (exponent == 0x1F) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else if (exponent) {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  } else if (mantissa) {
    float value = float(mantissa) * (1.0f / 16777216.0f);
    return sign ? -value : value;
  } else {
    bits = sign;
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

struct Half4 {
  float x, y, z, w;
};

Half4 ReadHalf4(const uint8_t* bytes) {
  const uint32_t a = ReadBigEndian(bytes);
  const uint32_t b = ReadBigEndian(bytes + 4);
  return {HalfToFloat(a & 0xFFFF), HalfToFloat(a >> 16), HalfToFloat(b & 0xFFFF),
          HalfToFloat(b >> 16)};
}

float SignedField(uint32_t packed, uint32_t offset, uint32_t width, bool no_zero) {
  const int32_t value = int32_t(packed << (32 - offset - width)) >> (32 - width);
  if (no_zero) {
    return (float(value) * 2.0f + 1.0f) * (1.0f / float((uint32_t(1) << width) - 1));
  }
  return std::max(float(value) * (1.0f / float((uint32_t(1) << (width - 1)) - 1)), -1.0f);
}

uint32_t PackSignedInteger(float value, uint32_t width, uint32_t offset) {
  if (std::isnan(value)) {
    value = 0.0f;
  }
  const float largest = float((uint32_t(1) << (width - 1)) - 1);
  value = std::min(std::max(value, -1.0f - largest), largest);
  const int32_t rounded = int32_t(value + std::copysign(0.5f, value));
  return (uint32_t(rounded) & ((uint32_t(1) << width) - 1)) << offset;
}

float XenosMul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

}  // namespace

bool ComputeFable2Skinning(const RegisterFile& regs, memory::Memory& memory,
                           const Shader& vertex_shader, std::vector<uint8_t>* capture,
                           uint32_t& written_start, uint32_t& written_length) {
  const uint32_t constant_base = regs.Get<reg::SQ_VS_CONST>().base;
  const auto is_stream = [&](const xenos::xe_gpu_memexport_stream_t& stream,
                             xenos::ColorFormat format, xenos::Endian128 endian,
                             xenos::SurfaceNumberFormat number_format) {
    return stream.const_0x1 == 0x1 && stream.const_0x4b0 == 0x4B0 && stream.const_0x96 == 0x96 &&
           stream.format == format && stream.endianness == endian &&
           stream.num_format == number_format && !stream.red_blue_swap;
  };
  const xenos::xe_gpu_memexport_stream_t position_stream =
      regs.GetMemExportStream(constant_base + 8);
  const xenos::xe_gpu_memexport_stream_t normal_stream =
      regs.GetMemExportStream(constant_base + 46);
  const xenos::xe_gpu_memexport_stream_t uv_stream = regs.GetMemExportStream(constant_base + 47);
  if (!is_stream(position_stream, xenos::ColorFormat::k_32_FLOAT, xenos::Endian128::k8in32,
                 xenos::SurfaceNumberFormat::kFloat) ||
      !is_stream(normal_stream, xenos::ColorFormat::k_2_10_10_10, xenos::Endian128::k8in32,
                 xenos::SurfaceNumberFormat::kSignedInteger) ||
      !is_stream(uv_stream, xenos::ColorFormat::k_16_16_FLOAT, xenos::Endian128::k8in16,
                 xenos::SurfaceNumberFormat::kFloat) ||
      position_stream.base_address != normal_stream.base_address ||
      position_stream.base_address != uv_stream.base_address) {
    return false;
  }

  const auto& bindings = vertex_shader.vertex_bindings();
  if (bindings.size() != 2 || bindings[0].stride_words != kVertexDwords ||
      bindings[1].stride_words != kBoneDwords) {
    return false;
  }
  bool normal_no_zero = false;
  for (const auto& attribute : bindings[0].attributes) {
    if (attribute.fetch_instr.attributes.data_format == xenos::VertexFormat::k_10_11_11) {
      normal_no_zero = attribute.fetch_instr.attributes.signed_rf_mode ==
                       xenos::SignedRepeatingFractionMode::kNoZero;
    }
  }
  const xenos::xe_gpu_vertex_fetch_t vertex_fetch = regs.GetVertexFetch(bindings[0].fetch_constant);
  const xenos::xe_gpu_vertex_fetch_t bone_fetch = regs.GetVertexFetch(bindings[1].fetch_constant);
  if (vertex_fetch.endian != xenos::Endian::k8in32 || bone_fetch.endian != xenos::Endian::k8in32) {
    return false;
  }

  const auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  const uint32_t vertex_count = draw_initiator.num_indices;
  const uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  if (draw_initiator.source_select != xenos::SourceSelect::kAutoIndex || !vertex_count ||
      vertex_count > kMaxVertices ||
      uint64_t(vertex_count + index_offset) * kOutputDwords > position_stream.index_count ||
      uint64_t(vertex_count + index_offset) * kVertexDwords > vertex_fetch.size) {
    return false;
  }
  const uint32_t bone_count = uint32_t(bone_fetch.size) / kBoneDwords;

  const uint8_t* vertices = memory.TranslatePhysical<const uint8_t*>(vertex_fetch.address << 2);
  const uint8_t* bones = memory.TranslatePhysical<const uint8_t*>(bone_fetch.address << 2);
  uint8_t* destination = memory.TranslatePhysical<uint8_t*>(
      uint32_t(position_stream.base_address + index_offset * kOutputDwords) << 2);
  if (!vertices || !bones || !destination) {
    return false;
  }

  // The bone matrices (three rows of four half floats each), decoded once
  // instead of for every vertex that uses them.
  static thread_local std::vector<Half4> bone_rows;
  bone_rows.resize(size_t(bone_count) * 3);
  for (uint32_t bone = 0; bone < bone_count; ++bone) {
    for (uint32_t row = 0; row < 3; ++row) {
      bone_rows[size_t(bone) * 3 + row] =
          ReadHalf4(bones + size_t(bone) * kBoneDwords * 4 + row * 8);
    }
  }

  static thread_local std::vector<uint8_t> output_scratch;
  std::vector<uint8_t> output_capture;
  std::vector<uint8_t>& output = capture ? output_capture : output_scratch;
  output.resize(size_t(vertex_count) * kOutputDwords * 4);
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const uint8_t* vertex = vertices + size_t(i + index_offset) * kVertexDwords * 4;
    const Half4 p = ReadHalf4(vertex);
    const uint32_t packed_normal = ReadBigEndian(vertex + 8);
    const float nx = SignedField(packed_normal, 0, 11, normal_no_zero);
    const float ny = SignedField(packed_normal, 11, 11, normal_no_zero);
    const float nz = SignedField(packed_normal, 22, 10, normal_no_zero);
    const uint32_t bone_indices = ReadBigEndian(vertex + 12);
    const uint32_t bone_weights = ReadBigEndian(vertex + 16);

    float position[3] = {};
    float normal[3] = {};
    for (uint32_t b = 0; b < 4; ++b) {
      const uint32_t bone = (bone_indices >> (b * 8)) & 0xFF;
      if (bone >= bone_count) {
        return false;
      }
      const float weight = float((bone_weights >> (b * 8)) & 0xFF) * (1.0f / 255.0f);
      const Half4* const bone_matrix = &bone_rows[size_t(bone) * 3];
      for (uint32_t row = 0; row < 3; ++row) {
        const Half4& m = bone_matrix[row];
        position[row] += (m.w * p.w + m.y * p.y + m.x * p.x + m.z) * weight;
        normal[row] += (nz * m.w + nx * m.y + ny * m.x) * weight;
      }
    }
    const float scale = 1.0f / std::sqrt(std::fabs(normal[0] * normal[0] + normal[2] * normal[2] +
                                                   normal[1] * normal[1]));
    const float out_x = XenosMul(normal[0], scale) * 511.0f;
    const float out_y = XenosMul(normal[1], scale) * 511.0f;
    const float out_z = XenosMul(normal[2], scale) * 511.0f;

    uint8_t* element = output.data() + size_t(i) * kOutputDwords * 4;
    for (uint32_t row = 0; row < 3; ++row) {
      uint32_t bits;
      std::memcpy(&bits, &position[row], sizeof(bits));
      WriteBigEndian(element + row * 4, bits);
    }
    // Decoding and re-encoding the two 16-bit floats keeps their bytes.
    std::memcpy(element + 12, vertex + 20, 4);
    WriteBigEndian(element + 16, PackSignedInteger(out_x, 10, 0) | PackSignedInteger(out_y, 10, 10) |
                                     PackSignedInteger(out_z, 10, 20) |
                                     PackSignedInteger(out_x, 2, 30));
  }

  if (capture) {
    *capture = std::move(output);
  } else {
    std::memcpy(destination, output.data(), output.size());
    written_start = uint32_t(position_stream.base_address + index_offset * kOutputDwords) << 2;
    written_length = uint32_t(output.size());
  }
  return true;
}

}  // namespace rex::graphics::fable2
