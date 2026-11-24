/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_HOST_TESSELLATION_H_
#define XENIA_GPU_METAL_METAL_HOST_TESSELLATION_H_

// Host tessellation route (OpenSpec implement-metal-host-tessellation, design
// D3/D6/D7): the PURE, device-free expansion of a tessellated guest draw into
// the per-vertex domain record stream + triangle index list the
// `domain_as_vertex` vertex shader consumes. The command processor stages the
// guest control-point index / factor stream into host order (SwapIndicesInto),
// calls Count* to size its upload-arena transients, then Write* to fill them.
// Everything here is a function of (shared ProcessingResult shape, guest
// registers, staged stream), so the L0 suite pins index resolution, patch
// layout and budget behaviour without a device.

#include <cstdint>
#include <cstring>

#include "rex/graphics/host_tessellator.h"
#include "rex/graphics/shader.h"
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// One generated vertex, 32 bytes at SV_VertexID * 32 in the
// xe_tessellation_vertices raw SRV (dxbc_shader_translator.cc,
// StartVertexOrDomainShader under domain_as_vertex).
struct XeTessellationVertex {
  // Direct3D SV_DomainLocation.xyz (triangle: u, v, w; quad: u, v, 0), w = 0.
  float domain_location[4];
  // vICP[i].x for i < control points: the resolved control-point indices
  // (CP-indexed shapes, 3 or 4 used) or the resolved patch index in [0]
  // (patch-indexed shapes), as floats, exactly as the canned hull shaders
  // produced them. Unused lanes are 0.
  float control_point_index[4];
};
static_assert(sizeof(XeTessellationVertex) == 32,
              "XeTessellationVertex must be 32 bytes (translator ld_raw ABI)");
constexpr uint32_t kXeTessellationVertexSizeBytes = 32;

// Per-draw expansion ceiling (design D7): an over-budget draw is a reason-coded
// skip, never a partial patch set or an unbounded allocation. 1 << 18 vertices
// = 8 MiB of records (+ up to 1.5 MiB of indices) inside the 32 MiB upload
// ring; VP's live ranges (max 8) put a 1024-patch quad draw at 82,944
// vertices. Re-derived from the live measurement at task 6.2.
constexpr uint32_t kMaxMetalHostTessellationVerticesPerDraw = 1u << 18;

// How the shared ::rex::gpu::PrimitiveProcessor's shape maps onto the guest stream and the
// record lanes (primitive_processor.cc:325-400 + the canned hull shaders).
struct MetalHostTessellationShape {
  HostTessellator::Domain domain;
  // kAdaptive: the guest "index buffer" holds float32 edge factors, one per
  // patch edge; the patch index comes from the patch ordinal.
  bool adaptive;
  // Triangle/quad LISTS: each patch is 3/4 control-point indices from the
  // stream (or sequential vertex ids when auto-indexed) and the guest shader
  // receives all of them. Triangle/quad PATCHES: the shader receives one
  // patch index.
  bool control_point_indexed;
  // Number of index lanes the record carries: 3 / 4 for CP-indexed, 1 for
  // patch-indexed.
  uint32_t control_points;
  // Guest stream words consumed per patch: adaptive 3 / 4 factors, CP-indexed
  // 3 / 4 indices, non-adaptive patch-indexed 1 index.
  uint32_t stream_words_per_patch;
};

inline bool DescribeMetalHostTessellationShape(
    ::rex::gpu::Shader::HostVertexShaderType type, xenos::TessellationMode mode,
    MetalHostTessellationShape* shape_out) {
  MetalHostTessellationShape shape = {};
  shape.adaptive = mode == xenos::TessellationMode::kAdaptive;
  switch (type) {
    case ::rex::gpu::Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      shape.domain = HostTessellator::Domain::kTriangle;
      shape.control_point_indexed = true;
      shape.control_points = 3;
      break;
    case ::rex::gpu::Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      shape.domain = HostTessellator::Domain::kQuad;
      shape.control_point_indexed = true;
      shape.control_points = 4;
      break;
    case ::rex::gpu::Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      shape.domain = HostTessellator::Domain::kTriangle;
      shape.control_point_indexed = false;
      shape.control_points = 1;
      break;
    case ::rex::gpu::Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      shape.domain = HostTessellator::Domain::kQuad;
      shape.control_point_indexed = false;
      shape.control_points = 1;
      break;
    default:
      return false;
  }
  // The shared processor only produces CP-indexed shapes for discrete /
  // continuous lists (adaptive lists are rejected upstream).
  if (shape.adaptive && shape.control_point_indexed) {
    return false;
  }
  uint32_t edges = shape.domain == HostTessellator::Domain::kTriangle ? 3 : 4;
  shape.stream_words_per_patch =
      shape.adaptive ? edges : (shape.control_point_indexed ? edges : 1);
  *shape_out = shape;
  return true;
}

// tessellation_indexed.vs.hlsl / adaptive_*.hs.hlsl:
//   float(clamp((index + xe_vertex_index_offset) & 0xFFFFFF,
//               xe_vertex_index_min_max.x, xe_vertex_index_min_max.y))
// with HLSL uint clamp == min(max(v, lo), hi), written out so lo > hi
// collapses to hi exactly as the shader does.
inline uint32_t ResolveMetalTessellationIndex(uint32_t raw_index,
                                              uint32_t index_offset,
                                              uint32_t min_index,
                                              uint32_t max_index) {
  uint32_t v = (raw_index + index_offset) & 0xFFFFFFu;
  v = v > min_index ? v : min_index;
  v = v < max_index ? v : max_index;
  return v;
}

struct MetalHostTessellationDrawInput {
  MetalHostTessellationShape shape;
  xenos::TessellationMode mode;
  // VGT_HOS_{MIN,MAX}_TESS_LEVEL + 1 (the published system-constant values).
  float factor_range_min;
  float factor_range_max;
  // VGT_INDX_OFFSET, VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX.
  uint32_t index_offset;
  uint32_t min_index;
  uint32_t max_index;
  // The guest stream in HOST order (after SwapIndicesInto), or nullptr for an
  // auto-indexed draw (stream word i == i). Adaptive streams are always
  // 32-bit float factors.
  const void* stream;
  bool stream_32bit;
  // Words in the stream / auto-indexed vertex count
  // (ProcessingResult::host_draw_vertex_count).
  uint32_t stream_word_count;

  uint32_t patch_count() const {
    return shape.stream_words_per_patch
               ? stream_word_count / shape.stream_words_per_patch
               : 0;
  }
  uint32_t stream_word(uint32_t i) const {
    if (!stream) {
      return i;
    }
    if (stream_32bit) {
      uint32_t v;
      std::memcpy(&v, static_cast<const uint8_t*>(stream) + i * 4, 4);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, static_cast<const uint8_t*>(stream) + i * 2, 2);
    return v;
  }
};

// Factors and record index lanes for one patch (design D5 + D6).
inline void ComputeMetalHostTessellationPatch(
    const MetalHostTessellationDrawInput& in, uint32_t patch,
    HostTessellationFactors* factors_out, float index_lanes_out[4]) {
  const MetalHostTessellationShape& shape = in.shape;
  uint32_t first_word = patch * shape.stream_words_per_patch;
  float guest_factors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (uint32_t k = 0; k < 4; ++k) {
    index_lanes_out[k] = 0.0f;
  }
  if (shape.adaptive) {
    for (uint32_t e = 0; e < shape.stream_words_per_patch; ++e) {
      uint32_t bits = in.stream_word(first_word + e);
      std::memcpy(&guest_factors[e], &bits, sizeof(float));
    }
    // adaptive_*.hs.hlsl: index = patch ordinal (SV_PrimitiveID).
    index_lanes_out[0] = float(ResolveMetalTessellationIndex(
        patch, in.index_offset, in.min_index, in.max_index));
  } else if (shape.control_point_indexed) {
    // tessellation_indexed.vs.hlsl per control point.
    for (uint32_t k = 0; k < shape.control_points; ++k) {
      index_lanes_out[k] = float(ResolveMetalTessellationIndex(
          in.stream_word(first_word + k), in.index_offset, in.min_index,
          in.max_index));
    }
  } else {
    // Non-adaptive patch list: one stream word per patch is the patch index.
    index_lanes_out[0] = float(ResolveMetalTessellationIndex(
        in.stream_word(first_word), in.index_offset, in.min_index,
        in.max_index));
  }
  *factors_out = HostTessellator::ComputeFactors(
      in.mode, shape.domain, in.factor_range_min, in.factor_range_max,
      shape.adaptive ? guest_factors : nullptr);
}

struct MetalHostTessellationCounts {
  uint32_t patch_count = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  bool over_budget = false;
};

// Pass 1: sizes the draw. Stops counting (over_budget) once the vertex ceiling
// is crossed so a hostile stream cannot make this pass itself expensive.
inline MetalHostTessellationCounts CountMetalHostTessellation(
    const MetalHostTessellationDrawInput& in, HostTessellator& tessellator,
    uint32_t vertex_budget = kMaxMetalHostTessellationVerticesPerDraw) {
  MetalHostTessellationCounts counts;
  counts.patch_count = in.patch_count();
  for (uint32_t p = 0; p < counts.patch_count; ++p) {
    HostTessellationFactors factors;
    float lanes[4];
    ComputeMetalHostTessellationPatch(in, p, &factors, lanes);
    const HostTessellationPattern& pattern =
        tessellator.Tessellate(in.mode, in.shape.domain, factors);
    counts.vertex_count += uint32_t(pattern.domain_points.size());
    counts.index_count += uint32_t(pattern.indices.size());
    if (counts.vertex_count > vertex_budget) {
      counts.over_budget = true;
      return counts;
    }
  }
  return counts;
}

// Pass 2: writes exactly Count*'s vertex_count records and index_count
// indices (patterns are re-looked-up per patch, so cache eviction between the
// passes cannot dangle a pointer; equal factor tuples yield equal patterns).
inline void WriteMetalHostTessellation(
    const MetalHostTessellationDrawInput& in, HostTessellator& tessellator,
    XeTessellationVertex* records, uint32_t* indices) {
  uint32_t patch_count = in.patch_count();
  uint32_t base_vertex = 0;
  uint32_t index_cursor = 0;
  for (uint32_t p = 0; p < patch_count; ++p) {
    HostTessellationFactors factors;
    float lanes[4];
    ComputeMetalHostTessellationPatch(in, p, &factors, lanes);
    const HostTessellationPattern& pattern =
        tessellator.Tessellate(in.mode, in.shape.domain, factors);
    for (size_t k = 0; k < pattern.domain_points.size(); ++k) {
      XeTessellationVertex& r = records[base_vertex + k];
      r.domain_location[0] = pattern.domain_points[k][0];
      r.domain_location[1] = pattern.domain_points[k][1];
      r.domain_location[2] = pattern.domain_points[k][2];
      r.domain_location[3] = 0.0f;
      r.control_point_index[0] = lanes[0];
      r.control_point_index[1] = lanes[1];
      r.control_point_index[2] = lanes[2];
      r.control_point_index[3] = lanes[3];
    }
    for (uint32_t i : pattern.indices) {
      indices[index_cursor++] = base_vertex + i;
    }
    base_vertex += uint32_t(pattern.domain_points.size());
  }
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_HOST_TESSELLATION_H_
