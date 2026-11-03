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

#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/fetch.h>

namespace rex::gpu {

struct SamplerState {
  xenos::TextureFilter min_filter;
  xenos::TextureFilter mag_filter;
  xenos::TextureFilter mip_filter;
  xenos::ClampMode clamp_u;
  xenos::ClampMode clamp_v;
  xenos::ClampMode clamp_w;
  xenos::AnisoFilter aniso_filter;
  xenos::BorderColor border_color;
  f32 lod_bias;
  u32 mip_min_level;
  u32 mip_max_level;

  static bool Prepare(const xenos::TextureFetch& fetch,
                      const ParsedTextureFetchInstruction& fetch_instr, SamplerState* out_info);

  u64 GetHash() const;
  bool operator==(const SamplerState& other) const {
    return min_filter == other.min_filter && mag_filter == other.mag_filter &&
           mip_filter == other.mip_filter && clamp_u == other.clamp_u && clamp_v == other.clamp_v &&
           clamp_w == other.clamp_w && aniso_filter == other.aniso_filter &&
           lod_bias == other.lod_bias && mip_min_level == other.mip_min_level &&
           mip_max_level == other.mip_max_level;
  }
};

}  // namespace rex::gpu
