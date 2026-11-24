/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_HOST_TESSELLATOR_H_
#define XENIA_GPU_HOST_TESSELLATOR_H_

// Host fixed-function tessellation for guest patch draws
// (OpenSpec change implement-metal-host-tessellation, design D2/D5/D7).
//
// A backend that cannot run the guest vertex shader as a domain shader feeds
// the guest patch stream through this class instead: it derives the
// Direct3D tessellation factors exactly as the shared canned hull shaders do
// (src/rex/graphics/shaders/{discrete,continuous,adaptive}_*.hs.hlsl), runs the
// vendored Direct3D 11 reference tessellator (thirdparty/d3d11-tessellator,
// the implementation every D3D11 hardware tessellator is specified to match),
// and returns the domain points and triangle connectivity of ONE patch as a
// cached pattern. The caller expands patterns into per-vertex records for the
// draw. No GPU stage is involved, so the result is fully inspectable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rex/graphics/xenos.h"

class CHWTessellator;

namespace rex {
namespace graphics {

// Direct3D SV_TessFactor / SV_InsideTessFactor order:
//   triangle: edges[0] = u==0 edge, [1] = v==0 edge, [2] = w==0 edge,
//             inside[0]; edges[3] and inside[1] unused (0).
//   quad:     edges[0] = u==0, [1] = v==0, [2] = u==1, [3] = v==1,
//             inside[0] along U, inside[1] along V.
struct HostTessellationFactors {
  std::array<float, 4> edges = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 2> inside = {0.0f, 0.0f};
  bool operator==(const HostTessellationFactors& other) const;
};

// The reference tessellator's output for one patch.
struct HostTessellationPattern {
  // Direct3D SV_DomainLocation: (u, v, 1-u-v) for triangles, (u, v, 0) for
  // quads. This is what the guest vertex shader translation swizzles from.
  std::vector<std::array<float, 3>> domain_points;
  // Triangle list into domain_points, clockwise in (u, v) as the reference
  // emits for PIPE_TESSELLATOR_OUTPUT_TRIANGLE_CW.
  std::vector<uint32_t> indices;
  // FNV-1a over the point bits and indices - the golden the L0 tests pin.
  uint64_t content_hash = 0;
  size_t triangle_count() const { return indices.size() / 3; }
};

class HostTessellator {
 public:
  enum class Domain : uint32_t {
    kTriangle,
    kQuad,
  };

  // Maximum tessellation factor the reference accepts; also bounds the
  // per-patch output ((64+1)^2 points, 64*64*2 triangles for a quad).
  static constexpr float kMaxTessellationFactor = 64.0f;
  static constexpr uint32_t kMaxPointsPerPatch = 65 * 65;
  static constexpr uint32_t kMaxTrianglesPerPatch = 64 * 64 * 2;

  explicit HostTessellator(size_t pattern_cache_capacity = 64);
  ~HostTessellator();
  HostTessellator(const HostTessellator&) = delete;
  HostTessellator& operator=(const HostTessellator&) = delete;

  // HLSL clamp(x, lo, hi) == min(max(x, lo), hi) with Direct3D min/max NaN
  // semantics (a NaN operand yields the other operand). Written out
  // explicitly, never via a clamp intrinsic, because the live Viva Pinata
  // range [2,1] (lo > hi) must collapse to hi exactly as the hull shaders'
  // clamp does (pre-gate design D2b).
  static float ClampFactor(float x, float lo, float hi);

  // Derives the Direct3D factors for one patch exactly as the canned hull
  // shaders do (design D5):
  //   discrete / continuous: every factor = factor_range_max;
  //   adaptive: guest_edge_factors[k] (host-endian float32 straight from the
  //             guest factor stream, one per patch edge, 3 or 4 used) become
  //             ClampFactor(f + 1, factor_range_min, factor_range_max), then
  //             triangle edges[i] = f[(i+1)%3], inside = min(edges);
  //             quad edges[i] = f[(i+3)&3], inside[0] = min(e1, e3),
  //             inside[1] = min(e0, e2).
  // factor_range_* are VGT_HOS_{MIN,MAX}_TESS_LEVEL + 1 (the system-constant
  // values). guest_edge_factors may be null for non-adaptive modes.
  static HostTessellationFactors ComputeFactors(
      xenos::TessellationMode mode, Domain domain, float factor_range_min,
      float factor_range_max, const float* guest_edge_factors);

  // Partitioning the canned hull shaders declare per guest mode: integer for
  // discrete, fractional_even for continuous and adaptive.
  static bool IsIntegerPartitioning(xenos::TessellationMode mode) {
    return mode == xenos::TessellationMode::kDiscrete;
  }

  // Returns the (cached) pattern for the factors. The reference is invoked
  // with the mode's partitioning, the domain, and clockwise triangles. The
  // returned reference stays valid until the pattern is evicted, which can
  // only happen after `pattern_cache_capacity` further distinct patterns.
  const HostTessellationPattern& Tessellate(
      xenos::TessellationMode mode, Domain domain,
      const HostTessellationFactors& factors);

  size_t pattern_cache_size() const { return cache_.size(); }
  size_t pattern_cache_capacity() const { return cache_capacity_; }
  uint64_t pattern_cache_misses() const { return cache_misses_; }

 private:
  struct CacheEntry {
    bool integer_partitioning;
    Domain domain;
    HostTessellationFactors factors;
    uint64_t last_use;
    std::unique_ptr<HostTessellationPattern> pattern;
  };

  void Generate(bool integer_partitioning, Domain domain,
                const HostTessellationFactors& factors,
                HostTessellationPattern& pattern_out);

  std::unique_ptr<CHWTessellator> tessellator_;
  std::vector<CacheEntry> cache_;
  size_t cache_capacity_;
  uint64_t use_counter_ = 0;
  uint64_t cache_misses_ = 0;
};

}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_HOST_TESSELLATOR_H_
