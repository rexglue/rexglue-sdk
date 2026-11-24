/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/host_tessellator.h"

#include <algorithm>
#include <cstring>

#include "thirdparty/d3d11-tessellator/tessellator.hpp"
#include "rex/assert.h"

namespace rex {
namespace graphics {

bool HostTessellationFactors::operator==(
    const HostTessellationFactors& other) const {
  // Bitwise, so that -0/+0 and NaN payloads are distinct keys - two patches
  // with bitwise-equal factors produce bitwise-equal patterns, nothing else
  // is promised.
  return std::memcmp(edges.data(), other.edges.data(), sizeof(edges)) == 0 &&
         std::memcmp(inside.data(), other.inside.data(), sizeof(inside)) == 0;
}

float HostTessellator::ClampFactor(float x, float lo, float hi) {
  // HLSL max(x, lo): returns lo when x is NaN (x > lo is false).
  float m = (x > lo) ? x : lo;
  // HLSL min(m, hi): returns hi when m is NaN (m < hi is false). Also yields
  // hi whenever lo > hi, which is the [2,1] collapse the hull shaders have.
  return (m < hi) ? m : hi;
}

HostTessellationFactors HostTessellator::ComputeFactors(
    xenos::TessellationMode mode, Domain domain, float factor_range_min,
    float factor_range_max, const float* guest_edge_factors) {
  HostTessellationFactors result;
  uint32_t edge_count = domain == Domain::kTriangle ? 3 : 4;
  if (mode != xenos::TessellationMode::kAdaptive) {
    // {discrete,continuous}_{triangle,quad}.hlsli: every edge and inside
    // factor is xe_tessellation_factor_range.y; the +1 and the CPU clamp are
    // already in the range.
    for (uint32_t i = 0; i < edge_count; ++i) {
      result.edges[i] = factor_range_max;
    }
    result.inside[0] = factor_range_max;
    if (domain == Domain::kQuad) {
      result.inside[1] = factor_range_max;
    }
    return result;
  }
  assert_not_null(guest_edge_factors);
  // tessellation_adaptive.vs.hlsl: clamp(asfloat(f) + 1.0, range.x, range.y).
  float converted[4] = {};
  for (uint32_t i = 0; i < edge_count; ++i) {
    converted[i] = ClampFactor(guest_edge_factors[i] + 1.0f, factor_range_min,
                               factor_range_max);
  }
  if (domain == Domain::kTriangle) {
    // adaptive_triangle.hs.hlsl: edges[i] = patch[(i + 1) % 3].edge_factor;
    // inside = min(min(e0, e1), e2).
    for (uint32_t i = 0; i < 3; ++i) {
      result.edges[i] = converted[(i + 1) % 3];
    }
    result.inside[0] = std::min(std::min(result.edges[0], result.edges[1]),
                                result.edges[2]);
  } else {
    // adaptive_quad.hs.hlsl: edges[i] = patch[(i + 3) & 3].edge_factor;
    // inside[0] = min(e1, e3); inside[1] = min(e0, e2).
    for (uint32_t i = 0; i < 4; ++i) {
      result.edges[i] = converted[(i + 3) & 3];
    }
    result.inside[0] = std::min(result.edges[1], result.edges[3]);
    result.inside[1] = std::min(result.edges[0], result.edges[2]);
  }
  return result;
}

HostTessellator::HostTessellator(size_t pattern_cache_capacity)
    : tessellator_(std::make_unique<CHWTessellator>()),
      cache_capacity_(std::max<size_t>(1, pattern_cache_capacity)) {
  cache_.reserve(cache_capacity_);
}

HostTessellator::~HostTessellator() = default;

const HostTessellationPattern& HostTessellator::Tessellate(
    xenos::TessellationMode mode, Domain domain,
    const HostTessellationFactors& factors) {
  bool integer_partitioning = IsIntegerPartitioning(mode);
  ++use_counter_;
  for (CacheEntry& entry : cache_) {
    if (entry.integer_partitioning == integer_partitioning &&
        entry.domain == domain && entry.factors == factors) {
      entry.last_use = use_counter_;
      return *entry.pattern;
    }
  }
  ++cache_misses_;
  CacheEntry* slot;
  if (cache_.size() < cache_capacity_) {
    cache_.emplace_back();
    slot = &cache_.back();
    slot->pattern = std::make_unique<HostTessellationPattern>();
  } else {
    // Evict the least recently used pattern; reuse its storage.
    slot = &*std::min_element(cache_.begin(), cache_.end(),
                              [](const CacheEntry& a, const CacheEntry& b) {
                                return a.last_use < b.last_use;
                              });
  }
  slot->integer_partitioning = integer_partitioning;
  slot->domain = domain;
  slot->factors = factors;
  slot->last_use = use_counter_;
  Generate(integer_partitioning, domain, factors, *slot->pattern);
  return *slot->pattern;
}

void HostTessellator::Generate(bool integer_partitioning, Domain domain,
                               const HostTessellationFactors& factors,
                               HostTessellationPattern& pattern_out) {
  // The canned hull shaders declare [partitioning("integer")] for discrete
  // and [partitioning("fractional_even")] for continuous/adaptive, always
  // [outputtopology("triangle_cw")].
  tessellator_->Init(integer_partitioning
                         ? PIPE_TESSELLATOR_PARTITIONING_INTEGER
                         : PIPE_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN,
                     PIPE_TESSELLATOR_OUTPUT_TRIANGLE_CW);
  if (domain == Domain::kTriangle) {
    tessellator_->TessellateTriDomain(factors.edges[0], factors.edges[1],
                                      factors.edges[2], factors.inside[0]);
  } else {
    tessellator_->TessellateQuadDomain(factors.edges[0], factors.edges[1],
                                       factors.edges[2], factors.edges[3],
                                       factors.inside[0], factors.inside[1]);
  }
  int point_count = tessellator_->GetPointCount();
  int index_count = tessellator_->GetIndexCount();
  assert_true(point_count >= 0 && index_count >= 0 && index_count % 3 == 0);
  const DOMAIN_POINT* points = tessellator_->GetPoints();
  const int* indices = tessellator_->GetIndices();

  pattern_out.domain_points.resize(size_t(point_count));
  for (int i = 0; i < point_count; ++i) {
    float u = points[i].u;
    float v = points[i].v;
    // Direct3D SV_DomainLocation: triangle barycentrics (u, v, w) with
    // w = 1 - u - v (tessellator.hpp DOMAIN_POINT comment); quad (u, v).
    pattern_out.domain_points[size_t(i)] = {
        u, v, domain == Domain::kTriangle ? 1.0f - u - v : 0.0f};
  }
  pattern_out.indices.resize(size_t(index_count));
  for (int i = 0; i < index_count; ++i) {
    assert_true(indices[i] >= 0 && indices[i] < point_count);
    pattern_out.indices[size_t(i)] = uint32_t(indices[i]);
  }

  // FNV-1a over the exact bits, so the golden pins the build's arithmetic.
  uint64_t hash = 0xCBF29CE484222325ull;
  auto mix = [&hash](const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 0x100000001B3ull;
    }
  };
  mix(pattern_out.domain_points.data(),
      pattern_out.domain_points.size() * sizeof(std::array<float, 3>));
  mix(pattern_out.indices.data(),
      pattern_out.indices.size() * sizeof(uint32_t));
  pattern_out.content_hash = hash;
}

}  // namespace gpu
}  // namespace rex
