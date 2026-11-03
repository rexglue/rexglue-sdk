/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/render_target/cache.h"
#include <rex/xenos/register.h>
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include <rex/logging/log.h>
#include <rex/bit.h>

namespace rex::gpu {

void RenderTargetCache::GetPSIColorClampsAndKeepMasks(xenos::ColorRenderTargetFormat format,
                                                      u32 write_mask, f32& clamp_rgb_low,
                                                      f32& clamp_alpha_low, f32& clamp_rgb_high,
                                                      f32& clamp_alpha_high, u32& keep_mask_low,
                                                      u32& keep_mask_high) {
  keep_mask_low = keep_mask_high = 0;
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (u32 i = 0; i < 4; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= static_cast<u32>(0xFF) << (i * 8);
        }
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (u32 i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= static_cast<u32>(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= static_cast<u32>(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = 31.875f;
      clamp_alpha_high = 1.0f;
      for (u32 i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= static_cast<u32>(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= static_cast<u32>(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      clamp_rgb_low = clamp_alpha_low = -32.0f;
      clamp_rgb_high = clamp_alpha_high = 32.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      clamp_rgb_low = clamp_alpha_low = -65504.0f;
      clamp_rgb_high = clamp_alpha_high = 65504.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high = std::nanf("");
      write_mask &= 0b0001;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~static_cast<u32>(0);
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high = std::nanf("");
      write_mask &= 0b0011;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~static_cast<u32>(0);
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_high = ~static_cast<u32>(0);
      }
      break;
    default:
      REX_UNHANDLED_CASE(format);
      write_mask = 0;
      break;
  }
  if (!write_mask) {
    keep_mask_low = keep_mask_high = ~static_cast<u32>(0);
  }
}

u32 RenderTargetCache::Transfer::GetRangeRectangles(u32 start_tiles, u32 end_tiles, u32 base_tiles,
                                                    u32 pitch_tiles,
                                                    xenos::MSAASamples msaa_samples, bool is_64bpp,
                                                    Rectangle* rectangles_out,
                                                    const Rectangle* cutout) {
  // EDRAM wrapping uses two clamped ranges, so start_tiles == end_tiles means an empty range.
  REX_ASSERT(start_tiles < xenos::kEDRAMTileCount);
  REX_ASSERT(end_tiles <= xenos::kEDRAMTileCount);
  REX_ASSERT(start_tiles <= end_tiles);
  // If start_tiles < base_tiles, this is the tail after EDRAM addressing wrapping.
  REX_ASSERT(start_tiles >= base_tiles || end_tiles <= base_tiles);
  REX_ASSERT(pitch_tiles != 0);
  if (start_tiles == end_tiles) {
    return 0;
  }
  u32 tile_width =
      xenos::kEDRAMTileWidthSamples >>
      (static_cast<u32>(msaa_samples >= xenos::MSAASamples::k4X) + static_cast<u32>(is_64bpp));
  u32 tile_height =
      xenos::kEDRAMTileHeightSamples >> static_cast<u32>(msaa_samples >= xenos::MSAASamples::k2X);
  u32 rectangle_count = 0;
  u32 local_offset = start_tiles < base_tiles ? xenos::kEDRAMTileCount : 0;
  u32 local_start = local_offset + start_tiles - base_tiles;
  u32 local_end = local_offset + end_tiles - base_tiles;
  // Inclusive.
  u32 rows_start = local_start / pitch_tiles;
  // Exclusive.
  u32 rows_end = (local_end + (pitch_tiles - 1)) / pitch_tiles;
  u32 row_first_start = local_start - rows_start * pitch_tiles;
  u32 row_last_end = pitch_tiles - (rows_end * pitch_tiles - local_end);
  u32 rows = rows_end - rows_start;
  if (rows == 1 || row_first_start) {
    Rectangle rectangle_first;
    rectangle_first.x_pixels = row_first_start * tile_width;
    rectangle_first.y_pixels = rows_start * tile_height;
    rectangle_first.width_pixels =
        ((rows == 1 ? row_last_end : pitch_tiles) - row_first_start) * tile_width;
    rectangle_first.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_first, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
    if (rows == 1) {
      return rectangle_count;
    }
  }
  u32 mid_rows_start = rows_start + 1;
  u32 mid_rows = rows - 2;
  if (!row_first_start) {
    --mid_rows_start;
    ++mid_rows;
  }
  if (row_last_end == pitch_tiles) {
    ++mid_rows;
  }
  if (mid_rows) {
    Rectangle rectangle_mid;
    rectangle_mid.x_pixels = 0;
    rectangle_mid.y_pixels = mid_rows_start * tile_height;
    rectangle_mid.width_pixels = pitch_tiles * tile_width;
    rectangle_mid.height_pixels = mid_rows * tile_height;
    rectangle_count += AddRectangle(
        rectangle_mid, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  if (row_last_end != pitch_tiles) {
    Rectangle rectangle_last;
    rectangle_last.x_pixels = 0;
    rectangle_last.y_pixels = (rows_end - 1) * tile_height;
    rectangle_last.width_pixels = row_last_end * tile_width;
    rectangle_last.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_last, rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  REX_ASSERT(rectangle_count <= (cutout ? kMaxRectanglesWithCutout : kMaxRectanglesWithoutCutout));
  return rectangle_count;
}

u32 RenderTargetCache::Transfer::AddRectangle(const Rectangle& rectangle, Rectangle* rectangles_out,
                                              const Rectangle* cutout) {
  u32 rectangle_right = rectangle.x_pixels + rectangle.width_pixels;
  u32 rectangle_bottom = rectangle.y_pixels + rectangle.height_pixels;
  if (!cutout || !cutout->width_pixels || !cutout->height_pixels ||
      cutout->x_pixels >= rectangle_right ||
      cutout->x_pixels + cutout->width_pixels <= rectangle.x_pixels ||
      cutout->y_pixels >= rectangle_bottom ||
      cutout->y_pixels + cutout->height_pixels <= rectangle.y_pixels) {
    if (rectangles_out) {
      rectangles_out[0] = rectangle;
    }
    return 1;
  }
  u32 rectangle_count = 0;
  u32 cutout_right = cutout->x_pixels + cutout->width_pixels;
  u32 cutout_bottom = cutout->y_pixels + cutout->height_pixels;
  if (cutout->y_pixels > rectangle.y_pixels) {
    REX_ASSERT(cutout->y_pixels < rectangle_bottom);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = rectangle.y_pixels;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      rectangle_upper.height_pixels = cutout->y_pixels - rectangle.y_pixels;
    }
    ++rectangle_count;
  }
  u32 middle_top = std::max(cutout->y_pixels, rectangle.y_pixels);
  u32 middle_height = std::min(cutout_bottom, rectangle_bottom) - middle_top;
  if (cutout->x_pixels > rectangle.x_pixels) {
    REX_ASSERT(cutout->x_pixels < rectangle_right);
    if (rectangles_out) {
      Rectangle& rectangle_middle_left = rectangles_out[rectangle_count];
      rectangle_middle_left.x_pixels = rectangle.x_pixels;
      rectangle_middle_left.y_pixels = middle_top;
      rectangle_middle_left.width_pixels = cutout->x_pixels - rectangle.x_pixels;
      rectangle_middle_left.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  if (cutout_right < rectangle_right) {
    REX_ASSERT(cutout_right > rectangle.x_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_middle_right = rectangles_out[rectangle_count];
      rectangle_middle_right.x_pixels = cutout_right;
      rectangle_middle_right.y_pixels = middle_top;
      rectangle_middle_right.width_pixels = rectangle_right - cutout_right;
      rectangle_middle_right.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  if (cutout_bottom < rectangle_bottom) {
    REX_ASSERT(cutout_bottom > rectangle.y_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = cutout_bottom;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      rectangle_upper.height_pixels = rectangle_bottom - cutout_bottom;
    }
    ++rectangle_count;
  }
  REX_ASSERT(rectangle_count <= kMaxCutoutBorderRectangles);
  return rectangle_count;
}

RenderTargetCache::~RenderTargetCache() {
  ShutdownCommon();
}

void RenderTargetCache::InitializeCommon() {
  REX_ASSERT(ownership_ranges_.empty());
  ownership_ranges_.emplace(std::piecewise_construct, std::forward_as_tuple(static_cast<u32>(0)),
                            std::forward_as_tuple(xenos::kEDRAMTileCount, RenderTargetKey(),
                                                  RenderTargetKey(), RenderTargetKey()));
}

void RenderTargetCache::DestroyAllRenderTargets() noexcept {
  ownership_ranges_.clear();
  render_targets_.clear();
}

void RenderTargetCache::ShutdownCommon() noexcept {
  DestroyAllRenderTargets();
}

void RenderTargetCache::ClearCache() {
  if (!render_targets_.empty()) {
    std::unordered_set<RenderTargetKey, RenderTargetKey::Hasher> used_render_targets;
    for (const auto& ownership_range_pair : ownership_ranges_) {
      const OwnershipRange& ownership_range = ownership_range_pair.second;
      if (!ownership_range.render_target.IsEmpty()) {
        used_render_targets.emplace(ownership_range.render_target);
      }
      if (!ownership_range.host_depth_render_target_unorm24.IsEmpty()) {
        used_render_targets.emplace(ownership_range.host_depth_render_target_unorm24);
      }
      if (!ownership_range.host_depth_render_target_float24.IsEmpty()) {
        used_render_targets.emplace(ownership_range.host_depth_render_target_float24);
      }
    }
    if (render_targets_.size() != used_render_targets.size()) {
      decltype(render_targets_)::iterator it_next;
      for (auto it = render_targets_.begin(); it != render_targets_.end(); it = it_next) {
        it_next = std::next(it);
        if (!it->second) {
          render_targets_.erase(it);
          continue;
        }
        if (used_render_targets.find(it->second->GetKey()) == used_render_targets.end()) {
          render_targets_.erase(it);
        }
      }
    }
  }
}

void RenderTargetCache::BeginFrame() {
  ResetAccumulatedRenderTargets();
}

bool RenderTargetCache::Update(bool is_rasterization_done,
                               xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                               u32 normalized_color_mask, const Shader& vertex_shader) {
  const xenos::RegisterFile& regs = GetRegisterFile();
  bool interlock_barrier_only = GetPath() == Path::kPixelShaderInterlock;

  auto rb_surface_info = regs.Get<xenos::reg::RB_SURFACE_INFO>();
  xenos::MSAASamples msaa_samples = rb_surface_info.msaa_samples;
  REX_ASSERT(msaa_samples <= xenos::MSAASamples::k4X);
  if (msaa_samples > xenos::MSAASamples::k4X) {
    REX_ASSERT_ALWAYS();
    REX_FAIL(gpu, false, "{}x MSAA requested by the guest, Xenos only supports up to 4x",
             static_cast<u32>(1) << static_cast<u32>(msaa_samples));
  }
  u32 msaa_samples_x_log2 = static_cast<u32>(msaa_samples >= xenos::MSAASamples::k4X);
  u32 pitch_pixels = rb_surface_info.surface_pitch;
  REX_ASSERT(pitch_pixels || !is_rasterization_done);
  if (!pitch_pixels) {
    is_rasterization_done = false;
  } else if (pitch_pixels > xenos::kTexture2DCubeMaxWidthHeight) {
    REX_FAIL(gpu, false,
             "Surface pitch {} larger than the maximum texture width {} specified "
             "by the guest",
             pitch_pixels, xenos::kTexture2DCubeMaxWidthHeight);
  }
  u32 pitch_tiles_at_32bpp =
      ((pitch_pixels << msaa_samples_x_log2) + (xenos::kEDRAMTileWidthSamples - 1)) /
      xenos::kEDRAMTileWidthSamples;
  if (!interlock_barrier_only) {
    u32 pitch_pixels_tile_aligned_scaled = pitch_tiles_at_32bpp *
                                           (xenos::kEDRAMTileWidthSamples >> msaa_samples_x_log2) *
                                           GetDrawResolutionScaleX();
    u32 max_render_target_width = GetMaxRenderTargetWidth();
    if (pitch_pixels_tile_aligned_scaled > max_render_target_width) {
      REX_FAIL(gpu, false,
               "Surface pitch aligned to EDRAM tiles and resolution scaled {} "
               "larger than the maximum host render target width {}",
               pitch_pixels_tile_aligned_scaled, max_render_target_width);
    }
  }

  // Index 0 is depth and stencil, 1 to 4 are color. Depth and stencil precede color.
  u32 depth_and_color_rts_used_bits = 0;
  u32 edram_bases[1 + xenos::kMaxColorRenderTargets];
  u32 resource_formats[1 + xenos::kMaxColorRenderTargets];
  u32 rts_are_64bpp = 0;
  if (is_rasterization_done) {
    if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable) {
      depth_and_color_rts_used_bits |= 1;
      auto rb_depth_info = regs.Get<xenos::reg::RB_DEPTH_INFO>();
      edram_bases[0] = rb_depth_info.depth_base;
      // With pixel shader interlock the addressing is the same regardless of the format.
      resource_formats[0] =
          interlock_barrier_only ? 0 : static_cast<u32>(rb_depth_info.depth_format);
    }
    for (u32 i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(normalized_color_mask & (static_cast<u32>(0b1111) << (4 * i)))) {
        continue;
      }
      auto color_info =
          regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRTRegisterIndices[i]);
      u32 rt_bit_index = 1 + i;
      depth_and_color_rts_used_bits |= static_cast<u32>(1) << rt_bit_index;
      edram_bases[rt_bit_index] = color_info.color_base;
      xenos::ColorRenderTargetFormat color_format =
          regs.Get<xenos::reg::RB_COLOR_INFO>(xenos::reg::RB_COLOR_INFO::kRTRegisterIndices[i])
              .color_format;
      bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_format);
      if (is_64bpp) {
        rts_are_64bpp |= static_cast<u32>(1) << rt_bit_index;
      }
      xenos::ColorRenderTargetFormat color_resource_format;
      if (interlock_barrier_only) {
        // Only address mapping and access overlap between draws matter, so only pixel size does.
        color_resource_format = is_64bpp ? xenos::ColorRenderTargetFormat::k_16_16_16_16
                                         : xenos::ColorRenderTargetFormat::k_8_8_8_8;
      } else {
        color_resource_format = GetColorResourceFormat(xenos::GetStorageColorFormat(color_format));
      }
      resource_formats[rt_bit_index] = static_cast<u32>(color_resource_format);
    }
  }

  u32 rts_remaining;
  u32 rt_index;

  rts_remaining = depth_and_color_rts_used_bits & ~(static_cast<u32>(1));
  while (rex::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(static_cast<u32>(1) << rt_index);
    u32 edram_base = edram_bases[rt_index];
    u32 rts_other_remaining =
        depth_and_color_rts_used_bits &
        (~((static_cast<u32>(1) << (rt_index + 1)) - 1) | static_cast<u32>(1));
    u32 rt_other_index;
    while (rex::bit_scan_forward(rts_other_remaining, &rt_other_index)) {
      rts_other_remaining &= ~(static_cast<u32>(1) << rt_other_index);
      if (edram_bases[rt_other_index] == edram_base) {
        depth_and_color_rts_used_bits &= ~(static_cast<u32>(1) << rt_other_index);
      }
    }
  }

  if (!interlock_barrier_only) {
    for (auto& transfers : last_update_transfers_) {
      transfers.clear();
    }
  }

  if (!depth_and_color_rts_used_bits) {
    std::ranges::fill(last_update_used_render_targets_, nullptr);
    if (are_accumulated_render_targets_valid_) {
      for (const RenderTarget* render_target : last_update_accumulated_render_targets_) {
        if (!render_target) {
          continue;
        }
        RenderTargetKey rt_key = render_target->GetKey();
        if (rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            rt_key.msaa_samples != msaa_samples) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    if (!are_accumulated_render_targets_valid_) {
      std::ranges::fill(last_update_accumulated_render_targets_, nullptr);
    }
    return true;
  }

  u32 height_used =
      std::min(GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples),
               draw_extent_estimator_.EstimateMaxY(
                   interlock_barrier_only
                       ? settings_.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend.Get()
                       : true,
                   vertex_shader));

  std::pair<u32, u32> edram_bases_sorted[1 + xenos::kMaxColorRenderTargets];
  u32 edram_bases_sorted_count = 0;
  rts_remaining = depth_and_color_rts_used_bits;
  while (rex::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(static_cast<u32>(1) << rt_index);
    edram_bases_sorted[edram_bases_sorted_count++] =
        std::make_pair(edram_bases[rt_index], rt_index);
  }
  std::sort(edram_bases_sorted, edram_bases_sorted + edram_bases_sorted_count);
  u32 rt_max_distance_tiles_at_64bpp = xenos::kEDRAMTileCount * 2;
  if (settings_.mrt_edram_used_range_clamp_to_min.Get() && edram_bases_sorted_count >= 2) {
    for (u32 i = 1; i < edram_bases_sorted_count; ++i) {
      const std::pair<u32, u32>& rt_base_prev = edram_bases_sorted[i - 1];
      rt_max_distance_tiles_at_64bpp =
          std::min(rt_max_distance_tiles_at_64bpp,
                   (edram_bases_sorted[i].first - rt_base_prev.first)
                       << (((rts_are_64bpp >> rt_base_prev.second) & 1) ^ 1));
    }
    const std::pair<u32, u32>& rt_base_last = edram_bases_sorted[edram_bases_sorted_count - 1];
    rt_max_distance_tiles_at_64bpp =
        std::min(rt_max_distance_tiles_at_64bpp,
                 (xenos::kEDRAMTileCount + edram_bases_sorted[0].first - rt_base_last.first)
                     << (((rts_are_64bpp >> rt_base_last.second) & 1) ^ 1));
  }

  RenderTargetKey rt_keys[1 + xenos::kMaxColorRenderTargets];
  RenderTarget* rts[1 + xenos::kMaxColorRenderTargets];
  u32 rt_lengths_tiles[1 + xenos::kMaxColorRenderTargets];
  u32 length_used_tiles_at_32bpp =
      ((height_used << static_cast<u32>(msaa_samples >= xenos::MSAASamples::k2X)) +
       (xenos::kEDRAMTileHeightSamples - 1)) /
      xenos::kEDRAMTileHeightSamples * pitch_tiles_at_32bpp;
  for (u32 i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<u32, u32>& rt_base_index = edram_bases_sorted[i];
    u32 rt_base = rt_base_index.first;
    u32 rt_bit_index = rt_base_index.second;
    RenderTargetKey& rt_key = rt_keys[rt_bit_index];
    rt_key.base_tiles = rt_base;
    rt_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    rt_key.msaa_samples = msaa_samples;
    rt_key.is_depth = rt_bit_index == 0;
    rt_key.resource_format = resource_formats[rt_bit_index];
    if (!interlock_barrier_only) {
      RenderTarget* render_target = GetOrCreateRenderTarget(rt_key);
      if (!render_target) {
        return false;
      }
      rts[rt_bit_index] = render_target;
    }
    u32 rt_is_64bpp = (rts_are_64bpp >> rt_bit_index) & 1;
    // The last target can extend to the first one's base (itself if alone), wrapping in EDRAM.
    rt_lengths_tiles[i] = std::min(std::min(length_used_tiles_at_32bpp << rt_is_64bpp,
                                            rt_max_distance_tiles_at_64bpp >> (rt_is_64bpp ^ 1)),
                                   ((i + 1 < edram_bases_sorted_count)
                                        ? edram_bases_sorted[i + 1].first
                                        : (xenos::kEDRAMTileCount + edram_bases_sorted[0].first)) -
                                       rt_base);
  }

  if (interlock_barrier_only) {
    // The full barrier may clear the ownership map, so check for an overlap needing it first.
    bool interlock_barrier_needed = false;
    for (u32 i = 0; i < edram_bases_sorted_count; ++i) {
      const std::pair<u32, u32>& rt_base_index = edram_bases_sorted[i];
      if (WouldOwnershipChangeRequireTransfers(rt_keys[rt_base_index.second], 0,
                                               rt_lengths_tiles[i])) {
        interlock_barrier_needed = true;
        break;
      }
    }
    if (interlock_barrier_needed) {
      RequestPixelShaderInterlockBarrier();
    }
  }

  // Transfers are assumed to succeed from here on, even if the implementation fails them.

  for (u32 i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<u32, u32>& rt_base_index = edram_bases_sorted[i];
    u32 rt_bit_index = rt_base_index.second;
    ChangeOwnership(rt_keys[rt_bit_index], 0, rt_lengths_tiles[i],
                    interlock_barrier_only ? nullptr : &last_update_transfers_[rt_bit_index]);
  }

  if (interlock_barrier_only) {
    return true;
  }

  for (u32 i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    last_update_used_render_targets_[i] =
        (depth_and_color_rts_used_bits & (static_cast<u32>(1) << i)) ? rts[i] : nullptr;
  }
  if (are_accumulated_render_targets_valid_) {
    for (u32 i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      RenderTarget* current_rt =
          (depth_and_color_rts_used_bits & (static_cast<u32>(1) << i)) ? rts[i] : nullptr;
      const RenderTarget* accumulated_rt = last_update_accumulated_render_targets_[i];
      if (!accumulated_rt) {
        if (current_rt) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
        last_update_accumulated_render_targets_[i] = current_rt;
        continue;
      }
      if (current_rt) {
        if (current_rt != accumulated_rt) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      } else {
        RenderTargetKey accumulated_rt_key = accumulated_rt->GetKey();
        if (accumulated_rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            accumulated_rt_key.msaa_samples != msaa_samples) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    for (u32 i = 1; are_accumulated_render_targets_valid_ && i < 1 + xenos::kMaxColorRenderTargets;
         ++i) {
      const RenderTarget* render_target = last_update_accumulated_render_targets_[i];
      if (!render_target) {
        continue;
      }
      for (u32 j = 0; j < i; ++j) {
        if (last_update_accumulated_render_targets_[j] == render_target) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
  }
  if (!are_accumulated_render_targets_valid_) {
    std::ranges::copy(last_update_used_render_targets_,
                      std::begin(last_update_accumulated_render_targets_));
    are_accumulated_render_targets_valid_ = true;
  }

  return true;
}

u32 RenderTargetCache::GetLastUpdateBoundRenderTargets(u32* depth_and_color_formats_out) const {
  if (GetPath() != Path::kHostRenderTargets) {
    if (depth_and_color_formats_out) {
      std::memset(depth_and_color_formats_out, 0,
                  sizeof(u32) * (1 + xenos::kMaxColorRenderTargets));
    }
    return 0;
  }
  u32 rts_used = 0;
  for (u32 i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    const RenderTarget* render_target = last_update_accumulated_render_targets_[i];
    if (!render_target) {
      if (depth_and_color_formats_out) {
        depth_and_color_formats_out[i] = 0;
      }
      continue;
    }
    rts_used |= static_cast<u32>(1) << i;
    if (depth_and_color_formats_out) {
      depth_and_color_formats_out[i] = render_target->GetKey().resource_format;
    }
  }
  return rts_used;
}

u32 RenderTargetCache::GetRenderTargetHeight(u32 pitch_tiles_at_32bpp,
                                             xenos::MSAASamples msaa_samples) const {
  if (!pitch_tiles_at_32bpp) {
    return 0;
  }
  // Down to the beginning of the render target in the next 11 bit EDRAM addressing period.
  u32 tile_rows = (xenos::kEDRAMTileCount + (pitch_tiles_at_32bpp - 1)) / pitch_tiles_at_32bpp;
  // Clamp to the guest limit (tile padding should exceed it) and host limit (padding must not).
  static_assert(!(xenos::kTexture2DCubeMaxWidthHeight % xenos::kEDRAMTileHeightSamples),
                "Maximum guest render target height is assumed to always be a multiple "
                "of an EDRAM tile height");
  u32 max_height_scaled = std::min(xenos::kTexture2DCubeMaxWidthHeight * GetDrawResolutionScaleY(),
                                   GetMaxRenderTargetHeight());
  u32 msaa_samples_y_log2 = static_cast<u32>(msaa_samples >= xenos::MSAASamples::k2X);
  u32 tile_height_samples_scaled = xenos::kEDRAMTileHeightSamples * GetDrawResolutionScaleY();
  tile_rows =
      std::min(tile_rows, (max_height_scaled << msaa_samples_y_log2) / tile_height_samples_scaled);
  REX_ASSERT(tile_rows != 0);
  return tile_rows * (xenos::kEDRAMTileHeightSamples >> msaa_samples_y_log2);
}

void RenderTargetCache::GetHostDepthStoreRectangleDispatch(
    const Transfer::Rectangle& transfer_rectangle, xenos::MSAASamples msaa_samples,
    HostDepthStoreRectangleConstant& rectangle_constant_out, u32& group_count_x_out,
    u32& group_count_y_out) const {
  HostDepthStoreRectangleConstant rectangle_constant;
  // The resolve granularity is 8 pixels, so clearing and tile size are aligned to 8.
  REX_ASSERT((transfer_rectangle.x_pixels & 7) == 0);
  REX_ASSERT((transfer_rectangle.y_pixels & 7) == 0);
  REX_ASSERT((transfer_rectangle.width_pixels & 7) == 0);
  REX_ASSERT((transfer_rectangle.height_pixels & 7) == 0);
  REX_ASSERT(transfer_rectangle.width_pixels != 0);
  rectangle_constant.x_pixels_div_8 = transfer_rectangle.x_pixels >> 3;
  rectangle_constant.y_pixels_div_8 = transfer_rectangle.y_pixels >> 3;
  rectangle_constant.width_pixels_div_8_minus_1 = (transfer_rectangle.width_pixels >> 3) - 1;
  rectangle_constant_out = rectangle_constant;
  // 1 thread group = 64x8 host samples.
  u32 pixel_size_x = GetDrawResolutionScaleX()
                     << static_cast<u32>(msaa_samples >= xenos::MSAASamples::k4X);
  u32 pixel_size_y = GetDrawResolutionScaleY()
                     << static_cast<u32>(msaa_samples >= xenos::MSAASamples::k2X);
  group_count_x_out = (transfer_rectangle.width_pixels * pixel_size_x + 63) >> 6;
  group_count_y_out = (transfer_rectangle.height_pixels * pixel_size_y) >> 3;
}

void RenderTargetCache::GetResolveCopyRectanglesToDump(
    u32 base, u32 row_length, u32 rows, u32 pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out) const {
  rectangles_out.clear();
  REX_ASSERT(row_length <= pitch);
  row_length = std::min(row_length, pitch);
  if (!row_length || !rows) {
    return;
  }
  auto get_rectangles_in_extent = [&](u32 extent_start, u32 extent_end, u32 range_local_offset) {
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.cbegin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.cend(); ++it) {
      u32 range_global_start = std::max(it->first, extent_start);
      if (range_global_start >= extent_end) {
        break;
      }
      RenderTargetKey rt_key = it->second.render_target;
      if (rt_key.IsEmpty()) {
        continue;
      }
      while (it != ownership_ranges_.cend()) {
        auto it_next = std::next(it);
        if (it_next == ownership_ranges_.cend() || it_next->first >= extent_end ||
            it_next->second.render_target != rt_key) {
          break;
        }
        it = it_next;
      }

      u32 range_local_start =
          range_local_offset + std::max(range_global_start, extent_start) - base;
      u32 range_local_end = range_local_offset + std::min(it->second.end_tiles, extent_end) - base;
      REX_ASSERT(range_local_start < range_local_end);

      u32 rows_start = range_local_start / pitch;
      u32 rows_end = (range_local_end + (pitch - 1)) / pitch;
      u32 row_first_start = range_local_start - rows_start * pitch;
      if (row_first_start >= row_length) {
        if (rows_start + 1 < rows_end) {
          ++rows_start;
          row_first_start = 0;
        } else {
          continue;
        }
      }

      auto it_rt = render_targets_.find(rt_key);
      REX_ASSERT(it_rt != render_targets_.cend());
      REX_ASSERT(it_rt->second != nullptr);
      rectangles_out.emplace_back(
          it_rt->second.get(), rows_start, rows_end - rows_start, row_first_start,
          std::min(pitch - (rows_end * pitch - range_local_end), row_length));
    }
  };
  u32 resolve_area_end = base + (rows - 1) * pitch + row_length;
  get_rectangles_in_extent(base, std::min(resolve_area_end, xenos::kEDRAMTileCount), 0);
  if (resolve_area_end > xenos::kEDRAMTileCount) {
    get_rectangles_in_extent(0, std::min(resolve_area_end & (xenos::kEDRAMTileCount - 1), base),
                             xenos::kEDRAMTileCount);
  }
}

void RenderTargetCache::GetResolveCopyDispatchesToDump(
    u32 base, u32 row_length, u32 rows, u32 pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out,
    std::vector<ResolveCopyDispatch>& dispatches_out) const {
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles_out);
  dispatches_out.clear();
  for (u32 rectangle_index = 0; rectangle_index < static_cast<u32>(rectangles_out.size());
       ++rectangle_index) {
    const ResolveCopyDumpRectangle& rectangle = rectangles_out[rectangle_index];
    ResolveCopyDumpRectangle::Dispatch
        rectangle_dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    u32 dispatch_count = rectangle.GetDispatches(pitch, row_length, rectangle_dispatches);
    for (u32 i = 0; i < dispatch_count; ++i) {
      dispatches_out.emplace_back(rectangle_index, rectangle_dispatches[i]);
    }
  }
}

bool RenderTargetCache::PrepareHostRenderTargetsResolveClear(
    const draw_state::ResolveRequest& resolve_info, Transfer::Rectangle& clear_rectangle_out,
    RenderTarget*& depth_render_target_out, std::vector<Transfer>& depth_transfers_out,
    RenderTarget*& color_render_target_out, std::vector<Transfer>& color_transfers_out) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);

  u32 pitch_tiles_at_32bpp;
  u32 base_offset_tiles_at_32bpp;
  xenos::MSAASamples msaa_samples;
  if (resolve_info.IsClearingDepth()) {
    pitch_tiles_at_32bpp = resolve_info.depth_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp =
        resolve_info.depth_edram_info.base_tiles - resolve_info.depth_original_base;
    msaa_samples = resolve_info.depth_edram_info.msaa_samples;
  } else if (resolve_info.IsClearingColor()) {
    pitch_tiles_at_32bpp = resolve_info.color_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp =
        resolve_info.color_edram_info.base_tiles - resolve_info.color_original_base;
    if (resolve_info.color_edram_info.format_is_64bpp) {
      REX_ASSERT((pitch_tiles_at_32bpp & 1) == 0);
      pitch_tiles_at_32bpp >>= 1;
      REX_ASSERT((base_offset_tiles_at_32bpp & 1) == 0);
      base_offset_tiles_at_32bpp >>= 1;
    }
    msaa_samples = resolve_info.color_edram_info.msaa_samples;
  } else {
    return false;
  }
  REX_ASSERT(msaa_samples <= xenos::MSAASamples::k4X);
  if (!pitch_tiles_at_32bpp) {
    return false;
  }
  u32 msaa_samples_x_log2 = static_cast<u32>(msaa_samples >= xenos::MSAASamples::k4X);
  u32 msaa_samples_y_log2 = static_cast<u32>(msaa_samples >= xenos::MSAASamples::k2X);
  if (pitch_tiles_at_32bpp > ((xenos::kTexture2DCubeMaxWidthHeight << msaa_samples_x_log2) +
                              (xenos::kEDRAMTileWidthSamples - 1)) /
                                 xenos::kEDRAMTileWidthSamples) {
    REX_FAIL(gpu, false,
             "Surface pitch in 80-sample groups {} at {}x MSAA larger than the "
             "maximum texture width {} specified by the guest in a resolve",
             pitch_tiles_at_32bpp, static_cast<u32>(1) << static_cast<u32>(msaa_samples),
             xenos::kTexture2DCubeMaxWidthHeight);
  }
  u32 pitch_pixels = pitch_tiles_at_32bpp * (xenos::kEDRAMTileWidthSamples >> msaa_samples_x_log2);
  u32 pitch_pixels_scaled = pitch_pixels * GetDrawResolutionScaleX();
  u32 max_render_target_width = GetMaxRenderTargetWidth();
  if (pitch_pixels_scaled > max_render_target_width) {
    REX_FAIL(gpu, false,
             "Surface pitch aligned to EDRAM tiles and resolution scaled {} larger "
             "than the maximum host render target width {} in a resolve",
             pitch_pixels_scaled, max_render_target_width);
  }

  u32 render_target_height_pixels = GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples);
  u32 base_offset_rows_at_32bpp = base_offset_tiles_at_32bpp / pitch_tiles_at_32bpp;
  Transfer::Rectangle clear_rectangle;
  clear_rectangle.x_pixels =
      std::min((base_offset_tiles_at_32bpp - base_offset_rows_at_32bpp * pitch_tiles_at_32bpp) *
                       (xenos::kEDRAMTileWidthSamples >> msaa_samples_x_log2) +
                   (static_cast<u32>(resolve_info.coordinate_info.edram_offset_x_div_8) << 3),
               pitch_pixels);
  clear_rectangle.y_pixels =
      std::min(base_offset_rows_at_32bpp * (xenos::kEDRAMTileHeightSamples >> msaa_samples_y_log2) +
                   (static_cast<u32>(resolve_info.coordinate_info.edram_offset_y_div_8) << 3),
               render_target_height_pixels);
  clear_rectangle.width_pixels =
      std::min(static_cast<u32>(resolve_info.coordinate_info.width_div_8) << 3,
               pitch_pixels - clear_rectangle.x_pixels);
  clear_rectangle.height_pixels = std::min(static_cast<u32>(resolve_info.height_div_8) << 3,
                                           render_target_height_pixels - clear_rectangle.y_pixels);
  if (!clear_rectangle.width_pixels || !clear_rectangle.height_pixels) {
    return false;
  }

  u32 clear_start_tiles_at_32bpp =
      ((clear_rectangle.y_pixels << msaa_samples_y_log2) / xenos::kEDRAMTileHeightSamples) *
          pitch_tiles_at_32bpp +
      (clear_rectangle.x_pixels << msaa_samples_x_log2) / xenos::kEDRAMTileWidthSamples;
  u32 clear_length_tiles_at_32bpp =
      (((clear_rectangle.y_pixels + clear_rectangle.height_pixels - 1) << msaa_samples_y_log2) /
       xenos::kEDRAMTileHeightSamples) *
          pitch_tiles_at_32bpp +
      ((clear_rectangle.x_pixels + clear_rectangle.width_pixels - 1) << msaa_samples_x_log2) /
          xenos::kEDRAMTileWidthSamples +
      1 - clear_start_tiles_at_32bpp;
  // The cleared range can reach from the base in the current 11 tile index bits to the next 11.
  u32 depth_clear_start_tiles_base_relative = 0;
  u32 depth_clear_length_tiles = 0;
  if (resolve_info.IsClearingDepth()) {
    depth_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp, xenos::kEDRAMTileCount);
    depth_clear_length_tiles =
        std::min(clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp, xenos::kEDRAMTileCount) -
        depth_clear_start_tiles_base_relative;
  }
  u32 color_clear_start_tiles_base_relative = 0;
  u32 color_clear_length_tiles = 0;
  if (resolve_info.IsClearingColor()) {
    color_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp << resolve_info.color_edram_info.format_is_64bpp,
                 xenos::kEDRAMTileCount);
    color_clear_length_tiles = std::min((clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp)
                                            << resolve_info.color_edram_info.format_is_64bpp,
                                        xenos::kEDRAMTileCount) -
                               color_clear_start_tiles_base_relative;
  }
  if (depth_clear_length_tiles && color_clear_length_tiles) {
    u32 depth_clear_start_tiles_wrapped =
        (resolve_info.depth_original_base + depth_clear_start_tiles_base_relative) &
        (xenos::kEDRAMTileCount - 1);
    u32 color_clear_start_tiles_wrapped =
        (resolve_info.color_original_base + color_clear_start_tiles_base_relative) &
        (xenos::kEDRAMTileCount - 1);
    depth_clear_length_tiles =
        std::min(depth_clear_length_tiles,
                 ((color_clear_start_tiles_wrapped < depth_clear_start_tiles_wrapped)
                      ? xenos::kEDRAMTileCount
                      : 0) +
                     color_clear_start_tiles_wrapped - depth_clear_start_tiles_wrapped);
    color_clear_length_tiles =
        std::min(color_clear_length_tiles,
                 ((depth_clear_start_tiles_wrapped < color_clear_start_tiles_wrapped)
                      ? xenos::kEDRAMTileCount
                      : 0) +
                     depth_clear_start_tiles_wrapped - color_clear_start_tiles_wrapped);
  }

  RenderTargetKey depth_render_target_key;
  RenderTarget* depth_render_target = nullptr;
  if (depth_clear_length_tiles) {
    depth_render_target_key.base_tiles = resolve_info.depth_original_base;
    depth_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    depth_render_target_key.msaa_samples = msaa_samples;
    depth_render_target_key.is_depth = 1;
    depth_render_target_key.resource_format = resolve_info.depth_edram_info.format;
    depth_render_target = GetOrCreateRenderTarget(depth_render_target_key);
    if (!depth_render_target) {
      depth_render_target_key = RenderTargetKey();
      depth_clear_length_tiles = 0;
    }
  }
  RenderTargetKey color_render_target_key;
  RenderTarget* color_render_target = nullptr;
  if (color_clear_length_tiles) {
    color_render_target_key.base_tiles = resolve_info.color_original_base;
    color_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    color_render_target_key.msaa_samples = msaa_samples;
    color_render_target_key.is_depth = 0;
    color_render_target_key.resource_format = static_cast<u32>(GetColorResourceFormat(
        static_cast<xenos::ColorRenderTargetFormat>(resolve_info.color_edram_info.format)));
    color_render_target = GetOrCreateRenderTarget(color_render_target_key);
    if (!color_render_target) {
      color_render_target_key = RenderTargetKey();
      color_clear_length_tiles = 0;
    }
  }
  if (!depth_clear_length_tiles && !color_clear_length_tiles) {
    return false;
  }

  clear_rectangle_out = clear_rectangle;
  depth_render_target_out = depth_render_target;
  depth_transfers_out.clear();
  if (depth_render_target) {
    ChangeOwnership(depth_render_target_key, depth_clear_start_tiles_base_relative,
                    depth_clear_length_tiles, &depth_transfers_out, &clear_rectangle);
  }
  color_render_target_out = color_render_target;
  color_transfers_out.clear();
  if (color_render_target) {
    ChangeOwnership(color_render_target_key, color_clear_start_tiles_base_relative,
                    color_clear_length_tiles, &color_transfers_out, &clear_rectangle);
  }
  return true;
}

void RenderTargetCache::PixelShaderInterlockFullEDRAMBarrierPlaced() {
  REX_ASSERT(GetPath() == Path::kPixelShaderInterlock);
  OwnershipRange empty_range(xenos::kEDRAMTileCount, RenderTargetKey(), RenderTargetKey(),
                             RenderTargetKey());
  if (ownership_ranges_.size() == 1) {
    REX_ASSERT(!ownership_ranges_.begin()->first);
    OwnershipRange& all_edram_range = ownership_ranges_.begin()->second;
    REX_ASSERT(all_edram_range.end_tiles == xenos::kEDRAMTileCount);
    all_edram_range = empty_range;
    return;
  }
  ownership_ranges_.clear();
  ownership_ranges_.emplace(0, empty_range);
}

RenderTargetCache::RenderTarget* RenderTargetCache::GetOrCreateRenderTarget(RenderTargetKey key) {
  REX_ASSERT(GetPath() == Path::kHostRenderTargets);
  auto it_rt = render_targets_.find(key);
  RenderTarget* render_target;
  if (it_rt != render_targets_.end()) {
    render_target = it_rt->second.get();
  } else {
    std::unique_ptr<RenderTarget> created = CreateRenderTarget(key);
    render_target = created.get();
    u32 width = key.GetWidth();
    u32 height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
    if (render_target) {
      REXLOG_DEBUG(gpu,
                   "Created a {}x{} {}xMSAA {} render target with guest format {} at "
                   "EDRAM base {}",
                   width, height, static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                   key.is_depth ? "depth" : "color", static_cast<u32>(key.resource_format),
                   static_cast<u32>(key.base_tiles));
    } else {
      REXLOG_ERROR(gpu,
                   "Failed to create a {}x{} {}xMSAA {} render target with guest format "
                   "{} at EDRAM base {}",
                   width, height, static_cast<u32>(1) << static_cast<u32>(key.msaa_samples),
                   key.is_depth ? "depth" : "color", static_cast<u32>(key.resource_format),
                   static_cast<u32>(key.base_tiles));
    }
    render_targets_.emplace(key, std::move(created));
  }
  return render_target;
}

bool RenderTargetCache::WouldOwnershipChangeRequireTransfers(RenderTargetKey dest,
                                                             u32 start_tiles_base_relative,
                                                             u32 length_tiles) const {
  // xenos::kEDRAMTileCount with length 0 is valid when both start and end are clamped to it.
  REX_ASSERT(start_tiles_base_relative <=
             (xenos::kEDRAMTileCount - static_cast<u32>(length_tiles != 0)));
  REX_ASSERT(length_tiles <= xenos::kEDRAMTileCount);
  if (length_tiles == 0) {
    return false;
  }
  bool host_depth_encoding_different = dest.is_depth && GetPath() == Path::kHostRenderTargets &&
                                       IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  auto would_require_transfers_in_extent = [&](u32 extent_start, u32 extent_end) -> bool {
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.end(); ++it) {
      if (it->first >= extent_end) {
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        continue;
      }
      RenderTargetKey transfer_source = it->second.render_target;
      if (!transfer_source.IsEmpty() && transfer_source != dest) {
        return true;
      }
    }
    return false;
  };
  // start_tiles_base_relative may be in the next 11 bit period, so wrap the start tile.
  u32 start_tiles = (dest.base_tiles + start_tiles_base_relative) & (xenos::kEDRAMTileCount - 1);
  u32 end_tiles = start_tiles + length_tiles;
  if (would_require_transfers_in_extent(start_tiles, std::min(end_tiles, xenos::kEDRAMTileCount))) {
    return true;
  }
  if (end_tiles > xenos::kEDRAMTileCount) {
    if (would_require_transfers_in_extent(
            0, std::min(end_tiles & (xenos::kEDRAMTileCount - 1), start_tiles))) {
      return true;
    }
  }
  return false;
}

void RenderTargetCache::ChangeOwnership(RenderTargetKey dest, u32 start_tiles_base_relative,
                                        u32 length_tiles,
                                        std::vector<Transfer>* transfers_append_out,
                                        const Transfer::Rectangle* resolve_clear_cutout) {
  // xenos::kEDRAMTileCount with length 0 is valid when both start and end are clamped to it.
  REX_ASSERT(start_tiles_base_relative <=
             (xenos::kEDRAMTileCount - static_cast<u32>(length_tiles != 0)));
  REX_ASSERT(length_tiles <= xenos::kEDRAMTileCount);
  if (length_tiles == 0) {
    return;
  }
  u32 dest_pitch_tiles = dest.GetPitchTiles();
  bool dest_is_64bpp = dest.Is64bpp();
  bool host_depth_encoding_different = dest.is_depth && GetPath() == Path::kHostRenderTargets &&
                                       IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  auto change_ownership_in_extent = [&](u32 extent_start, u32 extent_end) {
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start &&
          !it_pre->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        ownership_ranges_.emplace(extent_start, it_pre->second);
        it_pre->second.end_tiles = extent_start;
        it = std::next(it_pre);
      }
    }
    while (it != ownership_ranges_.end()) {
      if (it->first >= extent_end) {
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        ++it;
        continue;
      }
      if (it->second.end_tiles > extent_end) {
        ownership_ranges_.emplace(extent_end, it->second);
        it->second.end_tiles = extent_end;
      }
      if (transfers_append_out) {
        RenderTargetKey transfer_source = it->second.render_target;
        if (!transfer_source.IsEmpty() && transfer_source != dest) {
          u32 transfer_end_tiles = std::min(it->second.end_tiles, extent_end);
          if (!resolve_clear_cutout ||
              Transfer::GetRangeRectangles(it->first, transfer_end_tiles, dest.base_tiles,
                                           dest_pitch_tiles, dest.msaa_samples, dest_is_64bpp,
                                           nullptr, resolve_clear_cutout)) {
            RenderTargetKey transfer_host_depth_source =
                host_depth_encoding_different
                    ? it->second.GetHostDepthRenderTarget(dest.GetDepthFormat())
                    : RenderTargetKey();
            if (transfer_host_depth_source == transfer_source) {
              transfer_host_depth_source = RenderTargetKey();
            }
            if (!transfers_append_out->empty() &&
                transfers_append_out->back().end_tiles == it->first &&
                transfers_append_out->back().source->GetKey() == transfer_source &&
                ((transfers_append_out->back().host_depth_source == nullptr) ==
                 transfer_host_depth_source.IsEmpty()) &&
                (transfer_host_depth_source.IsEmpty() ||
                 transfers_append_out->back().host_depth_source->GetKey() ==
                     transfer_host_depth_source)) {
              transfers_append_out->back().end_tiles = transfer_end_tiles;
            } else {
              auto transfer_source_rt_it = render_targets_.find(transfer_source);
              if (transfer_source_rt_it != render_targets_.end()) {
                REX_ASSERT(transfer_source_rt_it->second != nullptr);
                auto transfer_host_depth_source_rt_it =
                    !transfer_host_depth_source.IsEmpty()
                        ? render_targets_.find(transfer_host_depth_source)
                        : render_targets_.end();
                if (transfer_host_depth_source.IsEmpty() ||
                    transfer_host_depth_source_rt_it != render_targets_.end()) {
                  REX_ASSERT(!(transfer_host_depth_source_rt_it != render_targets_.end() &&
                               !transfer_host_depth_source_rt_it->second));
                  transfers_append_out->emplace_back(
                      it->first, transfer_end_tiles, transfer_source_rt_it->second.get(),
                      transfer_host_depth_source_rt_it != render_targets_.end()
                          ? transfer_host_depth_source_rt_it->second.get()
                          : nullptr);
                }
              }
            }
          }
        }
      }
      it->second.render_target = dest;
      if (host_depth_encoding_different) {
        it->second.GetHostDepthRenderTarget(dest.GetDepthFormat()) = dest;
      }
      std::map<u32, OwnershipRange>::iterator it_next;
      if (it != ownership_ranges_.end()) {
        it_next = std::next(it);
        if (it_next != ownership_ranges_.end() && it_next->second.AreOwnersSame(it->second)) {
          it->second.end_tiles = it_next->second.end_tiles;
          auto it_after = std::next(it_next);
          ownership_ranges_.erase(it_next);
          it_next = it_after;
        }
      } else {
        it_next = ownership_ranges_.end();
      }
      // Merge with the previous range only after merging with the next, to get the correct end.
      if (it != ownership_ranges_.begin()) {
        auto it_prev = std::prev(it);
        if (it_prev->second.AreOwnersSame(it->second)) {
          it_prev->second.end_tiles = it->second.end_tiles;
          ownership_ranges_.erase(it);
        }
      }
      it = it_next;
    }
  };
  // start_tiles_base_relative may be in the next 11 bit period, so wrap the start tile.
  u32 start_tiles = (dest.base_tiles + start_tiles_base_relative) & (xenos::kEDRAMTileCount - 1);
  u32 end_tiles = start_tiles + length_tiles;
  change_ownership_in_extent(start_tiles, std::min(end_tiles, xenos::kEDRAMTileCount));
  if (end_tiles > xenos::kEDRAMTileCount) {
    change_ownership_in_extent(0, std::min(end_tiles & (xenos::kEDRAMTileCount - 1), start_tiles));
  }
}

}  // namespace rex::gpu
