/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

struct SamplerInfo {
  xenos::TextureFilter min_filter;
  xenos::TextureFilter mag_filter;
  xenos::TextureFilter mip_filter;
  xenos::ClampMode clamp_u;
  xenos::ClampMode clamp_v;
  xenos::ClampMode clamp_w;
  xenos::AnisoFilter aniso_filter;
  xenos::BorderColor border_color;
  float lod_bias;
  uint32_t mip_min_level;
  uint32_t mip_max_level;

  static bool Prepare(const xenos::xe_gpu_texture_fetch_t& fetch,
                      const ParsedTextureFetchInstruction& fetch_instr, SamplerInfo* out_info);

  uint64_t hash() const;
  bool operator==(const SamplerInfo& other) const {
    return min_filter == other.min_filter && mag_filter == other.mag_filter &&
           mip_filter == other.mip_filter && clamp_u == other.clamp_u && clamp_v == other.clamp_v &&
           clamp_w == other.clamp_w && aniso_filter == other.aniso_filter &&
           lod_bias == other.lod_bias && mip_min_level == other.mip_min_level &&
           mip_max_level == other.mip_max_level;
  }
};

}  // namespace rex::graphics
