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
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/graphics/diagnostic_util.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>

#include "prepared_cube.h"
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
#include "../../../../tests/internal_d3d12_texture_fixture/receipt.h"
namespace texture_fixture = rex::graphics::d3d12::fixture;
#endif

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_render_to_texture, 24, "GPU",
                     "Texture cache memory limit for render-to-texture (MB)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft, 384, "GPU",
                     "Soft texture cache memory limit (MB)")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_hard, 768, "GPU",
                     "Hard texture cache memory limit (MB)")
    .range(128, 8192)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft_lifetime, 30, "GPU",
                     "Soft texture cache memory limit lifetime (seconds)")
    .range(1, 3600);

REXCVAR_DEFINE_BOOL(gpu_3d_to_2d_texture, true, "GPU",
                    "Sample problematic 3D textures through 2D-compatible wrappers")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(texture_load_failure_log, false, "GPU",
                    "Log bounded receipts for failed texture loads (frame, key, site)")
    .audience(rex::cvar::Audience::kAdvanced)
    .persistence(rex::cvar::Persistence::kSessionOnly);

REXCVAR_DEFINE_BOOL(texture_load_failure_retry, false, "GPU",
                    "Retry a failed texture load on the next draw instead of the next frame")
    .audience(rex::cvar::Audience::kAdvanced)
    .persistence(rex::cvar::Persistence::kSessionOnly);

REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU",
                     "Forces anisotropic filtering for eligible textures.\n"
                     "Higher values keep textures sharper at oblique angles, but increase texture "
                     "sampling cost.\n"
                     " -1 = No override\n"
                     "  0 = Disable anisotropic filtering\n"
                     "  1 = Force 1x anisotropic filtering\n"
                     "  2 = Force 2x anisotropic filtering\n"
                     "  3 = Force 4x anisotropic filtering\n"
                     "  4 = Force 8x anisotropic filtering\n"
                     "  5 = Force 16x anisotropic filtering")
    .range(-1, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(draw_resolution_scale_x, 1, "GPU", "Draw resolution scale X (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(draw_resolution_scale_y, 1, "GPU", "Draw resolution scale Y (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(resolution_scale, 1, "Display",
                     "Resolution scale. Draw resolution scale for both X and Y axes (same as "
                     "setting draw_resolution_scale_x and draw_resolution_scale_y). 0 = auto: "
                     "choose an integer scale for the presented image height")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .audience(rex::cvar::Audience::kPlayer);

REXCVAR_DEFINE_BOOL(pre_mask_resolve_l2_block, true, "GPU",
                    "Pre-mask scaled resolve L2 blocks to the write range before iterating");

REXCVAR_DEFINE_STRING(texture_load_dump, "", "GPU",
                      "One-shot directory for a filtered texture-load DDS and metadata")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(texture_load_dump_filter, "", "GPU",
                      "Texture-load filter: hex_address, or "
                      "hex_address|*:width:height:pitch:tiled:format:endian")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// DEFINE_int32(
//     draw_resolution_scale_x, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "1, 2 and 3 may be supported, but support of anything above 1 depends on "
//     "the device properties, such as whether it supports sparse binding / tiled "
//     "resources, the number of virtual address bits per resource, and other "
//     "factors.\n"
//     "Various effects and parts of game rendering pipelines may work "
//     "incorrectly as pixels become ambiguous from the game's perspective and "
//     "because half-pixel offset (which normally doesn't affect coverage when "
//     "MSAA isn't used) becomes full-pixel.",
//     "GPU");
// DEFINE_int32(
//     draw_resolution_scale_y, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "See draw_resolution_scale_x for more information.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft, 384,
//     "Maximum host texture memory usage (in megabytes) above which old textures "
//     "will be destroyed.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft_lifetime, 30,
//     "Seconds a texture should be unused to be considered old enough to be "
//     "deleted if texture memory usage exceeds texture_cache_memory_limit_soft.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_hard, 768,
//     "Maximum host texture memory usage (in megabytes) above which textures "
//     "will be destroyed as soon as possible.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_render_to_texture, 24,
//     "Part of the host texture memory budget (in megabytes) that will be scaled "
//     "by the current drawing resolution scale.\n"
//     "If texture_cache_memory_limit_soft, for instance, is 384, and this is 24, "
//     "it will be assumed that the game will be using roughly 24 MB of "
//     "render-to-texture (resolve) targets and 384 - 24 = 360 MB of regular "
//     "textures - so with 2x2 resolution scaling, the soft limit will be 360 + "
//     "96 MB, and with 3x3, it will be 360 + 216 MB.",
//     "GPU");

namespace rex::graphics {

bool IsPreparedCubeFetch(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      fetch.dimension != xenos::DataDimension::kCube || fetch.stacked ||
      fetch.size_2d.stack_depth != 5 || fetch.tiled || !fetch.packed_mips ||
      fetch.mip_min_level != 0 || fetch.exp_adjust || fetch.force_bc_w_to_max ||
      !fetch.base_address || !fetch.mip_address || fetch.base_address == fetch.mip_address) {
    return false;
  }
  const bool small = fetch.size_2d.width == 63 && fetch.size_2d.height == 63 && fetch.pitch == 2 &&
                     fetch.mip_max_level == 6;
  const bool bc3 = fetch.size_2d.width == 127 && fetch.size_2d.height == 127 && fetch.pitch == 4 &&
                   fetch.mip_max_level == 7 && fetch.format == xenos::TextureFormat::k_DXT4_5 &&
                   fetch.endianness == xenos::Endian::k8in16 && fetch.swizzle == 0x688 &&
                   !fetch.num_format && !(fetch.dword_0 & 0x3FC);
  const bool rgba8 = fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
                     fetch.endianness == xenos::Endian::k8in32 && fetch.swizzle == 0xA0A &&
                     !fetch.num_format && fetch.sign_x == xenos::TextureSign::kUnsigned &&
                     fetch.sign_y == xenos::TextureSign::kUnsigned &&
                     fetch.sign_z == xenos::TextureSign::kUnsigned &&
                     fetch.sign_w == xenos::TextureSign::kUnsigned;
  const bool rgba16f = fetch.format == xenos::TextureFormat::k_16_16_16_16_FLOAT &&
                       fetch.endianness == xenos::Endian::k8in16 && fetch.swizzle == 0x688 &&
                       fetch.num_format == 1 && fetch.sign_x == xenos::TextureSign::kSigned &&
                       fetch.sign_y == xenos::TextureSign::kSigned &&
                       fetch.sign_z == xenos::TextureSign::kSigned &&
                       fetch.sign_w == xenos::TextureSign::kSigned;
  return (small && (rgba8 || rgba16f)) || bc3;
}

namespace {

// Limit detailed texture_load_failure_log lines per frame and summarize the rest.
constexpr uint32_t kMaxLoadFailureLogsPerFrame = 8;

// A cache's loads and BeginFrame share its command processor thread, so thread-local
// state belongs to that cache without changing the installed layout.
struct LoadFailureState {
  uint64_t frame_index = 0;
  uint64_t total = 0;
  uint64_t last_submission = 0;
  uint32_t frame_count = 0;
  bool log = false;
  bool retry = false;
};
thread_local LoadFailureState load_failure_state;

std::atomic<uint64_t> next_texture_instance_id{1};

// Prepared native resolves use complete tiled allocations, including
// padding/guard tiles, for this captured float form. Keep the correction in the
// read-only Prepared view; ordinary metadata uses TextureGuestLayout.
bool IsPreparedSignedRg16Float(const xenos::xe_gpu_texture_fetch_t& fetch,
                               const diagnostic::DrawCaptureFrameInputTextureView& view,
                               uint8_t swizzled_signs) {
  // This captured producer shape includes tiled padding and the guard tile. Tie
  // correction to its complete source-backed descriptor; do not admit another
  // single-level RG16 fetch by coincidence.
  constexpr uint32_t kWidth = 1280;
  constexpr uint32_t kHeight = 720;
  constexpr uint32_t kPitch = 40;
  constexpr uint32_t kSwizzle = 0xB48;
  constexpr uint8_t kAllSigned = uint8_t(xenos::TextureSign::kSigned) * uint8_t(0b01010101);
  return view.is_signed && swizzled_signs == kAllSigned &&
         fetch.format == xenos::TextureFormat::k_16_16_FLOAT &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.width + 1 == kWidth && fetch.size_2d.height + 1 == kHeight &&
         fetch.pitch == kPitch && fetch.swizzle == kSwizzle && fetch.tiled && !fetch.packed_mips &&
         fetch.mip_min_level == 0 && fetch.mip_max_level == 0 &&
         fetch.sign_x == xenos::TextureSign::kSigned &&
         fetch.sign_y == xenos::TextureSign::kSigned &&
         fetch.sign_z == xenos::TextureSign::kSigned && fetch.sign_w == xenos::TextureSign::kSigned;
}

// Exact tiled CPU-writer forms whose legacy visible ranges omit tile storage.
bool IsPreparedUnsignedTiledBc(const xenos::xe_gpu_texture_fetch_t& fetch,
                               const diagnostic::DrawCaptureFrameInputTextureView& view,
                               uint8_t signs) {
  struct Form {
    xenos::TextureFormat format;
    uint32_t width, height, pitch, max_level;
    bool packed;
  };
  constexpr Form kForms[] = {
      {xenos::TextureFormat::k_DXT1, 128, 64, 4, 7, true},
      {xenos::TextureFormat::k_DXT1, 256, 128, 8, 8, true},
      {xenos::TextureFormat::k_DXT2_3, 128, 64, 4, 7, true},
      {xenos::TextureFormat::k_DXT2_3, 32, 64, 4, 0, false},
      {xenos::TextureFormat::k_DXT4_5, 128, 128, 4, 7, true},
  };
  const bool selected = std::any_of(std::begin(kForms), std::end(kForms), [&](const auto& form) {
    return fetch.format == form.format && fetch.size_2d.width + 1 == form.width &&
           fetch.size_2d.height + 1 == form.height && fetch.pitch == form.pitch &&
           fetch.mip_max_level == form.max_level && fetch.packed_mips == form.packed &&
           (form.max_level ? (fetch.mip_address & 0x1FFFF) != 0 : fetch.mip_address == 0);
  });
  return selected && !view.is_signed && signs == 0 && fetch.tiled &&
         fetch.endianness == xenos::Endian::kNone && fetch.swizzle == 0x688 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.stack_depth == 0 && fetch.mip_min_level == 0 && !fetch.num_format &&
         !fetch.exp_adjust && !fetch.force_bc_w_to_max &&
         fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned;
}

bool IsPreparedUnsignedBc1Packed(const xenos::xe_gpu_texture_fetch_t& fetch,
                                 const diagnostic::DrawCaptureFrameInputTextureView& view,
                                 uint8_t swizzled_signs) {
  // Exact packed tuples only. Single-level ranges use the ordinary coverage guard.
  constexpr uint32_t kForms[][4] = {
      {64, 64, 4, 6},    {128, 128, 4, 7}, {256, 256, 8, 8},  {256, 128, 8, 8},
      {512, 512, 16, 9}, {32, 32, 4, 5},   {512, 256, 16, 9}, {1024, 1024, 32, 10},
  };
  const bool selected = std::any_of(std::begin(kForms), std::end(kForms), [&](const auto& form) {
    return fetch.size_2d.width + 1 == form[0] && fetch.size_2d.height + 1 == form[1] &&
           fetch.pitch == form[2] && fetch.mip_max_level == form[3];
  });
  return selected && !view.is_signed && swizzled_signs == 0 &&
         fetch.format == xenos::TextureFormat::k_DXT1 &&
         fetch.endianness == xenos::Endian::k8in16 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.swizzle == 0x688 && !fetch.tiled && fetch.packed_mips && fetch.mip_min_level == 0 &&
         fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned;
}

bool IsPreparedUnsignedBc2Packed(const xenos::xe_gpu_texture_fetch_t& fetch,
                                 const diagnostic::DrawCaptureFrameInputTextureView& view,
                                 uint8_t swizzled_signs) {
  // Exact packed tuples only. Single-level ranges use the ordinary coverage guard.
  constexpr uint32_t kForms[][4] = {
      {64, 64, 4, 6},   {128, 128, 4, 7}, {128, 256, 4, 8},  {128, 64, 4, 7},
      {256, 128, 8, 8}, {256, 256, 8, 8}, {512, 256, 16, 9}, {512, 512, 16, 9},
  };
  const bool selected = std::any_of(std::begin(kForms), std::end(kForms), [&](const auto& form) {
    return fetch.size_2d.width + 1 == form[0] && fetch.size_2d.height + 1 == form[1] &&
           fetch.pitch == form[2] && fetch.mip_max_level == form[3];
  });
  return selected && !view.is_signed && swizzled_signs == 0 &&
         fetch.format == xenos::TextureFormat::k_DXT2_3 &&
         fetch.endianness == xenos::Endian::k8in16 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.swizzle == 0x688 && !fetch.tiled && fetch.packed_mips && fetch.mip_min_level == 0 &&
         fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned;
}

bool IsPreparedUnsignedBc3Packed(const xenos::xe_gpu_texture_fetch_t& fetch,
                                 const diagnostic::DrawCaptureFrameInputTextureView& view,
                                 uint8_t swizzled_signs) {
  // Exact packed tuples only. Single-level ranges use the ordinary coverage guard.
  constexpr uint32_t kForms[][4] = {
      {64, 64, 4, 6},   {128, 128, 4, 7},  {256, 128, 8, 8},
      {256, 256, 8, 8}, {512, 256, 16, 9}, {512, 512, 16, 9},
  };
  const bool selected = std::any_of(std::begin(kForms), std::end(kForms), [&](const auto& form) {
    return fetch.size_2d.width + 1 == form[0] && fetch.size_2d.height + 1 == form[1] &&
           fetch.pitch == form[2] && fetch.mip_max_level == form[3];
  });
  return selected && !view.is_signed && swizzled_signs == 0 &&
         fetch.format == xenos::TextureFormat::k_DXT4_5 &&
         fetch.endianness == xenos::Endian::k8in16 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.stack_depth == 0 && !fetch.num_format && !fetch.exp_adjust &&
         !fetch.force_bc_w_to_max && fetch.swizzle == 0x688 && !fetch.tiled && fetch.packed_mips &&
         fetch.mip_min_level == 0 && fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned;
}

// Correct only the exact unsigned CPU forms whose legacy ranges omit a tail.
// A zero mip page retains the reference's effective LOD0 normalization.
bool IsPreparedUnsignedRgba8R8Packed(const xenos::xe_gpu_texture_fetch_t& fetch,
                                     const diagnostic::DrawCaptureFrameInputTextureView& view,
                                     uint8_t signs) {
  const bool r8 = fetch.format == xenos::TextureFormat::k_8 && fetch.size_2d.width == 511 &&
                  fetch.size_2d.height == 511 && fetch.pitch == 16 && fetch.mip_max_level == 9 &&
                  (fetch.mip_address & 0x1FFFF) != 0 && fetch.endianness == xenos::Endian::kNone &&
                  fetch.swizzle == 0x124;
  const bool rgba8 = fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
                     fetch.size_2d.width == 255 &&
                     (fetch.size_2d.height == 3 || fetch.size_2d.height == 0) && fetch.pitch == 8 &&
                     fetch.mip_max_level == 8 && fetch.mip_address == 0 &&
                     fetch.endianness == xenos::Endian::k8in32 && fetch.swizzle == 0x60A;
  return (r8 || rgba8) && !view.is_signed && signs == 0 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.stack_depth == 0 && !fetch.tiled && fetch.packed_mips &&
         fetch.mip_min_level == 0 && !fetch.num_format && !fetch.exp_adjust &&
         !fetch.force_bc_w_to_max && fetch.sign_x == xenos::TextureSign::kUnsigned &&
         fetch.sign_y == xenos::TextureSign::kUnsigned &&
         fetch.sign_z == xenos::TextureSign::kUnsigned &&
         fetch.sign_w == xenos::TextureSign::kUnsigned;
}

struct TextureLoadDumpFilter {
  uint32_t address;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t tiled;
  uint32_t format;
  uint32_t endian;
};

bool ParseTextureLoadDumpFilter(const std::string& value, TextureLoadDumpFilter& filter_out,
                                bool& exact_out, bool& any_address_out) {
  if (value.find(':') == std::string::npos) {
    auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), filter_out.address, 16);
    exact_out = false;
    any_address_out = false;
    return !value.empty() && error == std::errc() && end == value.data() + value.size();
  }

  uint32_t* fields[] = {&filter_out.address, &filter_out.width, &filter_out.height,
                        &filter_out.pitch,   &filter_out.tiled, &filter_out.format,
                        &filter_out.endian};
  std::string_view remaining = value;
  for (size_t i = 0; i < std::size(fields); ++i) {
    size_t separator = remaining.find(':');
    std::string_view field = remaining.substr(0, separator);
    if (field.empty() || (i + 1 != std::size(fields)) != (separator != std::string_view::npos)) {
      return false;
    }
    if (i == 0 && field == "*") {
      any_address_out = true;
      if (separator != std::string_view::npos) {
        remaining.remove_prefix(separator + 1);
      }
      continue;
    }
    auto [end, error] =
        std::from_chars(field.data(), field.data() + field.size(), *fields[i], i == 0 ? 16 : 10);
    if (error != std::errc() || end != field.data() + field.size()) {
      return false;
    }
    if (separator != std::string_view::npos) {
      remaining.remove_prefix(separator + 1);
    }
  }
  exact_out = true;
  return true;
}

#pragma pack(push, 1)
// Narrow DDS output adapted from:
// https://github.com/birabittoh/rexglue-sdk/commit/164b2229eaae9e4d38d3c7dc9a74c38498d3b6b9
struct DdsPixelFormat {
  uint32_t size = 32;
  uint32_t flags = 0x41;
  uint32_t four_cc = 0;
  uint32_t rgb_bit_count = 32;
  uint32_t r_bit_mask = 0x000000FF;
  uint32_t g_bit_mask = 0x0000FF00;
  uint32_t b_bit_mask = 0x00FF0000;
  uint32_t a_bit_mask = 0xFF000000;
};

struct DdsHeader {
  uint32_t magic = 0x20534444;
  uint32_t size = 124;
  uint32_t flags = 0x100F;
  uint32_t height = 0;
  uint32_t width = 0;
  uint32_t pitch = 0;
  uint32_t depth = 0;
  uint32_t mip_map_count = 1;
  uint32_t reserved[11] = {};
  DdsPixelFormat pixel_format;
  uint32_t caps = 0x1000;
  uint32_t caps2 = 0;
  uint32_t caps3 = 0;
  uint32_t caps4 = 0;
  uint32_t reserved2 = 0;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 128);

bool DumpTextureLoad(const TextureLoadDumpFilter& filter, const std::filesystem::path& directory,
                     const uint8_t* source, uint32_t source_size) {
  if (!source || !source_size || filter.format != uint32_t(xenos::TextureFormat::k_8_8_8_8)) {
    return false;
  }

  const FormatInfo* format_info = FormatInfo::Get(xenos::TextureFormat::k_8_8_8_8);
  uint32_t pitch_blocks = filter.pitch / format_info->block_width;
  uint32_t width_blocks = filter.width / format_info->block_width;
  uint32_t height_blocks = filter.height / format_info->block_height;
  uint32_t output_pitch = width_blocks * format_info->bytes_per_block();
  std::vector<uint8_t> output(size_t(output_pitch) * height_blocks);
  xenos::Endian endian = xenos::Endian(filter.endian);
  if (filter.tiled) {
    texture_conversion::UntileInfo untile_info{};
    untile_info.width = width_blocks;
    untile_info.height = height_blocks;
    untile_info.input_pitch = pitch_blocks;
    untile_info.output_pitch = width_blocks;
    untile_info.input_format_info = format_info;
    untile_info.output_format_info = format_info;
    untile_info.copy_callback = [endian](void* output_block, const void* input_block,
                                         size_t length) {
      texture_conversion::CopySwapBlock(endian, output_block, input_block, length);
    };
    texture_conversion::Untile(output.data(), source, &untile_info);
  } else {
    uint32_t input_pitch = pitch_blocks * format_info->bytes_per_block();
    for (uint32_t y = 0; y < height_blocks; ++y) {
      texture_conversion::CopySwapBlock(endian, output.data() + size_t(y) * output_pitch,
                                        source + size_t(y) * input_pitch, output_pitch);
    }
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return false;
  }
  std::string stem =
      fmt::format("texture_{:08x}_{}x{}_p{}_t{}_f{}_e{}", filter.address, filter.width,
                  filter.height, filter.pitch, filter.tiled, filter.format, filter.endian);
  std::filesystem::path dds_path = directory / stem;
  dds_path.replace_extension(".dds");
  std::filesystem::path metadata_path = directory / stem;
  metadata_path.replace_extension(".txt");
  std::filesystem::path dds_temporary_path = dds_path;
  dds_temporary_path += ".tmp";
  std::filesystem::path metadata_temporary_path = metadata_path;
  metadata_temporary_path += ".tmp";
  auto remove_temporary_file = [](const std::filesystem::path& path) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    if (remove_error) {
      REXGPU_ERROR("Failed to clean up diagnostic temporary file {}: {}", rex::path_to_utf8(path),
                   remove_error.message());
      return false;
    }
    return true;
  };
  auto remove_temporary_pair = [&]() {
    bool dds_removed = remove_temporary_file(dds_temporary_path);
    bool metadata_removed = remove_temporary_file(metadata_temporary_path);
    return dds_removed && metadata_removed;
  };
  if (!remove_temporary_pair()) {
    return false;
  }

  DdsHeader dds_header;
  dds_header.width = filter.width;
  dds_header.height = filter.height;
  dds_header.pitch = output_pitch;
  std::ofstream dds(dds_temporary_path, std::ios::binary | std::ios::trunc);
  dds.write(reinterpret_cast<const char*>(&dds_header), sizeof(dds_header));
  dds.write(reinterpret_cast<const char*>(output.data()), output.size());
  dds.close();
  if (!dds) {
    remove_temporary_pair();
    return false;
  }

  size_t source_nonzero = 0;
  for (uint32_t i = 0; i < source_size; ++i) {
    source_nonzero += source[i] != 0;
  }
  size_t output_nonzero = 0;
  for (uint8_t byte : output) {
    output_nonzero += byte != 0;
  }
  std::ofstream metadata(metadata_temporary_path, std::ios::trunc);
  metadata << "address=0x" << std::hex << filter.address << std::dec << '\n'
           << "width=" << filter.width << '\n'
           << "height=" << filter.height << '\n'
           << "pitch=" << filter.pitch << '\n'
           << "tiled=" << filter.tiled << '\n'
           << "format=" << filter.format << '\n'
           << "endian=" << filter.endian << '\n'
           << "source_bytes=" << source_size << '\n'
           << "source_nonzero_bytes=" << source_nonzero << '\n'
           << "decoded_bytes=" << output.size() << '\n'
           << "decoded_nonzero_bytes=" << output_nonzero << '\n';
  metadata.close();
  if (!metadata) {
    remove_temporary_pair();
    return false;
  }

  const diagnostic::ArtifactPairPublicationResult publication = diagnostic::PublishArtifactPair(
      dds_temporary_path, metadata_temporary_path, dds_path, metadata_path);
  if (publication.cleanup_error) {
    REXGPU_ERROR("Failed to clean up diagnostic artifact temporary files: {}",
                 publication.cleanup_error.message());
  }
  if (!publication.succeeded()) {
    REXGPU_ERROR("Failed to publish diagnostic artifact pair: {}",
                 publication.publication_error.message());
    return false;
  }

  REXGPU_INFO("Dumped first matching texture load to {} ({} of {} source bytes nonzero)",
              rex::path_to_utf8(dds_path), source_nonzero, source_size);
  return true;
}

}  // namespace

const TextureCache::LoadShaderInfo TextureCache::load_shader_info_[kLoadShaderCount] = {
    // k8bpb
    {3, 4, 1, 4},
    // k16bpb
    {4, 4, 2, 4},
    // k32bpb
    {4, 4, 4, 3},
    // k64bpb
    {4, 4, 8, 2},
    // k128bpb
    {4, 4, 16, 1},
    // kR5G5B5A1ToB5G5R5A1
    {4, 4, 2, 4},
    // kR5G6B5ToB5G6R5
    {4, 4, 2, 4},
    // kR5G6B5ToRGBA8
    {4, 4, 4, 4},
    // kR5G5B6ToB5G6R5WithRBGASwizzle
    {4, 4, 2, 4},
    // kRGBA4ToBGRA4
    {4, 4, 2, 4},
    // kRGBA4ToARGB4
    {4, 4, 2, 4},
    // kRGBA4ToRGBA8
    {4, 4, 4, 4},
    // kGBGR8ToGRGB8
    {4, 4, 4, 3},
    // kGBGR8ToRGB8
    {4, 4, 8, 3},
    // kBGRG8ToRGBG8
    {4, 4, 4, 3},
    // kBGRG8ToRGB8
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR16UNormToFloat
    {4, 4, 2, 4},
    // kR16SNormToFloat
    {4, 4, 2, 4},
    // kRG16UNormToFloat
    {4, 4, 4, 3},
    // kRG16SNormToFloat
    {4, 4, 4, 3},
    // kRGBA16UNormToFloat
    {4, 4, 8, 2},
    // kRGBA16SNormToFloat
    {4, 4, 8, 2},
    // kDXT1ToRGBA8
    {4, 4, 4, 2},
    // kDXT3ToRGBA8
    {4, 4, 4, 1},
    // kDXT5ToRGBA8
    {4, 4, 4, 1},
    // kDXNToRG8
    {4, 4, 2, 1},
    // kDXT3A
    {4, 4, 1, 2},
    // kDXT3AAs1111ToBGRA4
    {4, 4, 2, 2},
    // kDXT3AAs1111ToARGB4
    {4, 4, 2, 2},
    // kDXT5AToR8
    {4, 4, 1, 2},
    // kCTX1
    {4, 4, 2, 2},
    // kDepthUnorm
    {4, 4, 4, 3},
    // kDepthFloat
    {4, 4, 4, 3},
};

TextureCache::TextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                           uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {
  assert_true(draw_resolution_scale_x >= 1);
  assert_true(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  assert_true(draw_resolution_scale_y >= 1);
  assert_true(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  texture_load_dump_config_.directory = REXCVAR_GET(texture_load_dump);
  std::string texture_load_dump_filter = REXCVAR_GET(texture_load_dump_filter);
  if (!texture_load_dump_config_.directory.empty() || !texture_load_dump_filter.empty()) {
    TextureLoadDumpFilter parsed_filter{};
    bool exact_filter = true;
    bool any_address = false;
    if (texture_load_dump_config_.directory.empty() ||
        !ParseTextureLoadDumpFilter(texture_load_dump_filter, parsed_filter, exact_filter,
                                    any_address)) {
      texture_load_dump_attempted_ = true;
      REXGPU_ERROR("Invalid texture load dump configuration");
    } else {
      texture_load_dump_config_.address = parsed_filter.address;
      texture_load_dump_config_.width = parsed_filter.width;
      texture_load_dump_config_.height = parsed_filter.height;
      texture_load_dump_config_.pitch = parsed_filter.pitch;
      texture_load_dump_config_.tiled = parsed_filter.tiled;
      texture_load_dump_config_.format = parsed_filter.format;
      texture_load_dump_config_.endian = parsed_filter.endian;
      texture_load_dump_config_.any_address = any_address;
      texture_load_dump_config_.exact = exact_filter;
      texture_load_dump_config_.enabled = true;
      if (any_address) {
        REXGPU_INFO("Armed one-shot shape-only texture load dump");
      } else {
        REXGPU_INFO("Armed one-shot {} texture load dump for 0x{:08X}",
                    exact_filter ? "exact" : "address-only", parsed_filter.address);
      }
    }
  }

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr uint32_t kScaledResolvePageDwordCount = SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ = std::unique_ptr<uint32_t[]>(new uint32_t[kScaledResolvePageDwordCount]);
    std::memset(scaled_resolve_pages_.get(), 0, kScaledResolvePageDwordCount * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ =
        shared_memory.RegisterGlobalWatch(ScaledResolveGlobalWatchCallbackThunk, this);
  }
}

TextureCache::~TextureCache() {
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    shared_memory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
}

uint32_t AutoDrawResolutionScale(uint32_t surface_width, uint32_t surface_height,
                                 uint32_t guest_width, uint32_t guest_height, uint32_t max_scale) {
  if (!surface_width || !surface_height || !guest_width || !guest_height) {
    return 1;
  }
  // The guest-aspect image fitted inside the surface is limited by the tighter axis.
  const uint64_t scale = std::min((uint64_t(surface_height) + guest_height - 1) / guest_height,
                                  (uint64_t(surface_width) + guest_width - 1) / guest_width);
  return uint32_t(std::clamp<uint64_t>(scale, 1, std::max<uint32_t>(1, max_scale)));
}

bool TextureCache::GetConfigDrawResolutionScale(uint32_t& x_out, uint32_t& y_out,
                                                const DisplayGeometry& display,
                                                DrawResolutionScaleRequest* request_out) {
  const int32_t shared_value = REXCVAR_GET(resolution_scale);
  // Auto applies even as an application default, which HasNonDefaultValue reports as default.
  const bool shared_auto = shared_value <= 0;
  const bool use_shared_scale = shared_auto || rex::cvar::HasNonDefaultValue("resolution_scale");
  const uint32_t shared_scale =
      shared_auto ? AutoDrawResolutionScale(display.surface_width, display.surface_height,
                                            display.guest_width, display.guest_height,
                                            kMaxDrawResolutionScaleAlongAxis)
                  : uint32_t(std::max(INT32_C(1), shared_value));
  auto axis_scale = [&](const char* axis_name, int32_t axis_value, bool& auto_out) {
    if (use_shared_scale && !rex::cvar::HasNonDefaultValue(axis_name)) {
      auto_out = shared_auto;
      return shared_scale;
    }
    auto_out = false;
    return uint32_t(std::max(INT32_C(1), axis_value));
  };
  DrawResolutionScaleRequest request;
  const uint32_t config_x =
      axis_scale("draw_resolution_scale_x", REXCVAR_GET(draw_resolution_scale_x), request.auto_x);
  const uint32_t config_y =
      axis_scale("draw_resolution_scale_y", REXCVAR_GET(draw_resolution_scale_y), request.auto_y);
  request.x = config_x;
  request.y = config_y;
  if (request_out) {
    *request_out = request;
  }
  uint32_t clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  uint32_t clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

void TextureCache::LogDisplayScale(const DisplayGeometry& display,
                                   const DrawResolutionScaleRequest& request, uint32_t effective_x,
                                   uint32_t effective_y) {
  auto axis_text = [](bool is_auto, uint32_t value) {
    return is_auto ? std::string("auto") : std::to_string(value);
  };
  const std::string requested =
      request.auto_x && request.auto_y
          ? std::string("auto")
          : axis_text(request.auto_x, request.x) + "x" + axis_text(request.auto_y, request.y);
  REXGPU_INFO("Display: guest={}x{} scale_requested={} scale_effective={}x{} surface={}x{}",
              display.guest_width, display.guest_height, requested, effective_x, effective_y,
              display.surface_width, display.surface_height);
}

void TextureCache::ClearCache() {
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  texture_fixture::TraceLifecycle(this, 6);
#endif
  DestroyAllTextures();
}

void TextureCache::CompletedSubmissionUpdated(uint64_t completed_submission_index) {
  // If memory usage is too high, destroy unused textures.
  uint64_t current_time = rex::chrono::Clock::QueryHostUptimeMillis();
  // texture_cache_memory_limit_render_to_texture is assumed to be included in
  // texture_cache_memory_limit_soft and texture_cache_memory_limit_hard, at 1x,
  // so subtracting 1 from the scale.
  uint32_t limit_scaled_resolve_add_mb =
      REXCVAR_GET(texture_cache_memory_limit_render_to_texture) *
      (draw_resolution_scale_x() * draw_resolution_scale_y() - 1);
  uint32_t limit_soft_mb =
      REXCVAR_GET(texture_cache_memory_limit_soft) + limit_scaled_resolve_add_mb;
  uint32_t limit_hard_mb =
      REXCVAR_GET(texture_cache_memory_limit_hard) + limit_scaled_resolve_add_mb;
  uint32_t limit_soft_lifetime = REXCVAR_GET(texture_cache_memory_limit_soft_lifetime) * 1000;
  bool destroyed_any = false;
  while (texture_used_first_ != nullptr) {
    uint64_t total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->last_usage_submission_index() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded && (texture->last_usage_time() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // The texture being destroyed might have been bound in the previous
      // submissions, and nothing has overwritten the binding yet, so completion
      // of the submission where the texture was last actually used on the GPU
      // doesn't imply that it's not bound currently. Reset bindings if
      // any texture has been destroyed.
      ResetTextureBindings();
    }
    // Remove the texture from the map and destroy it via its unique_ptr.
    auto found_texture_it = textures_.find(texture->key());
    assert_true(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      assert_true(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
      // `texture` is invalid now.
    }
  }
  if (destroyed_any) {
    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  }
}

void TextureCache::BeginSubmission(uint64_t new_submission_index) {
  assert_true(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = rex::chrono::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  LoadFailureState& failures = load_failure_state;
  if (failures.frame_count > kMaxLoadFailureLogsPerFrame) {
    REXGPU_WARN("Texture load failures: frame={} last_submission={} total={} suppressed={}",
                failures.frame_index, failures.last_submission, failures.total,
                failures.frame_count - kMaxLoadFailureLogsPerFrame);
  }
  failures.frame_count = 0;
  ++failures.frame_index;
  failures.log = REXCVAR_GET(texture_load_failure_log);
  failures.retry = REXCVAR_GET(texture_load_failure_retry);
  // In case there was a failure to create something in the previous frame, make
  // sure bindings are reset so a new attempt will surely be made if the texture
  // is requested again.
  ResetTextureBindings();
}

void TextureCache::MarkRangeAsResolved(uint32_t start_unscaled, uint32_t length_unscaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (IsDrawResolutionScaled()) {
    uint32_t page_first = start_unscaled >> 12;
    uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
    uint32_t block_first = page_first >> 5;
    uint32_t block_last = page_last >> 5;
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint32_t add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      scaled_resolve_pages_[i] |= add_bits;
      scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
    }
  }

  // Invalidate textures. Toggling individual textures between scaled and
  // unscaled also relies on invalidation through shared memory.
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  texture_fixture::TraceDeclaration(this, start_unscaled, length_unscaled,
                                    IsDrawResolutionScaled());
#endif
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
}

uint32_t TextureCache::GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t host_swizzle_component;
    if (guest_swizzle_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
      // something broken) the simple way - by changing them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component = (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

bool TextureCache::PrepareTextureLoad(Texture& texture, PendingTextureLoad& pending_load_out,
                                      PendingSharedMemoryRange* pending_ranges_out,
                                      size_t& pending_range_count_out) {
  uint32_t outdated_mask = texture.outdated_mask();
  if (!outdated_mask) {
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
    texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kNoop, texture);
#endif
    return false;
  }

  bool base_outdated = false;
  bool mips_outdated = false;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (outdated_mask & Texture::kOutdatedBitBase) {
      base_outdated = texture.base_outdated(global_lock);
    }
    if (outdated_mask & Texture::kOutdatedBitMips) {
      mips_outdated = texture.mips_outdated(global_lock);
    }
  }
  if (!base_outdated && !mips_outdated) {
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
    texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kNoop, texture);
#endif
    return false;
  }

  pending_load_out.texture = &texture;
  pending_load_out.load_base = base_outdated;
  pending_load_out.load_mips = mips_outdated;
  pending_range_count_out = 0;

  // Watch before requesting shared memory so a guest write after the request
  // has inspected or uploaded the range still marks this texture outdated.
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kPrepareBegin, texture,
                                base_outdated, mips_outdated);
#endif
  texture.MakeUpToDateAndWatch(global_critical_region_.Acquire(), base_outdated, mips_outdated);
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kPrepare, texture, base_outdated,
                                mips_outdated);
#endif

  TextureKey texture_key = texture.key();
  if (base_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.base_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestBaseSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }
  if (mips_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.mip_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestMipsSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }

  return true;
}

bool TextureCache::CommitPreparedTextureLoad(const PendingTextureLoad& pending_load) {
  if (!pending_load.texture || (!pending_load.load_base && !pending_load.load_mips)) {
    return true;
  }

  Texture& texture = *pending_load.texture;
  TextureKey texture_key = texture.key();
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  const uint64_t fixture_entry_generation = texture.content_generation();
  bool fixture_backend_attempted = false;
#endif
  auto restore_outdated = [&]() {
    texture.RestoreOutdatedAfterLoadFailure(global_critical_region_.Acquire(),
                                            pending_load.load_base, pending_load.load_mips);
    OnTextureLoadFailure(LoadFailureSite::kPrepare, texture, pending_load.load_base,
                         pending_load.load_mips);
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
    texture_fixture::TraceTexture(
        this,
        fixture_backend_attempted ? texture_fixture::TraceKind::kBackendFailure
                                  : texture_fixture::TraceKind::kResidencyFailure,
        texture, pending_load.load_base, pending_load.load_mips, fixture_entry_generation);
#endif
    return false;
  };
  if (texture_key.scaled_resolve) {
    // Make sure all the scaled resolve memory is resident and accessible from
    // the shader, including any possible padding that hasn't yet been touched
    // by an actual resolve, but is still included in the texture size, so the
    // GPU won't be trying to access unmapped memory.
    if (pending_load.load_base && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.base_page << 12, texture.GetGuestBaseSize(), 4)) {
      return restore_outdated();
    }
    if (pending_load.load_mips && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.mip_page << 12, texture.GetGuestMipsSize(), 4)) {
      return restore_outdated();
    }
  }

#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  fixture_backend_attempted = true;
  texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kBackendAttempt, texture,
                                pending_load.load_base, pending_load.load_mips,
                                fixture_entry_generation);
#endif
  if (!LoadTextureDataFromResidentMemoryImpl(texture, pending_load.load_base,
                                             pending_load.load_mips)) {
    return restore_outdated();
  }

#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
  texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kLoadSuccess, texture,
                                pending_load.load_base, pending_load.load_mips,
                                fixture_entry_generation);
#endif

  if (texture_load_dump_config_.enabled && !texture_load_dump_attempted_ &&
      pending_load.load_base && !texture_key.scaled_resolve &&
      (texture_load_dump_config_.any_address ||
       (uint32_t(texture_key.base_page) << 12) == texture_load_dump_config_.address) &&
      (!texture_load_dump_config_.exact ||
       (texture_key.GetWidth() == texture_load_dump_config_.width &&
        texture_key.GetHeight() == texture_load_dump_config_.height &&
        (uint32_t(texture_key.pitch) << 5) == texture_load_dump_config_.pitch &&
        texture_key.tiled == texture_load_dump_config_.tiled &&
        uint32_t(texture_key.format) == texture_load_dump_config_.format &&
        uint32_t(texture_key.endianness) == texture_load_dump_config_.endian))) {
    TextureLoadDumpFilter dump_filter{
        uint32_t(texture_key.base_page) << 12, texture_key.GetWidth(), texture_key.GetHeight(),
        uint32_t(texture_key.pitch) << 5,      texture_key.tiled,      uint32_t(texture_key.format),
        uint32_t(texture_key.endianness),
    };
    REXGPU_INFO("Matched one-shot texture load dump: 0x{:08X}:{}:{}:{}:{}:{}:{}",
                dump_filter.address, dump_filter.width, dump_filter.height, dump_filter.pitch,
                dump_filter.tiled, dump_filter.format, dump_filter.endian);
    const uint8_t* source = shared_memory().TranslatePhysical(texture_key.base_page << 12);
    if (DumpTextureLoad(dump_filter, rex::to_path(texture_load_dump_config_.directory), source,
                        texture.GetGuestBaseSize())) {
      texture_load_dump_attempted_ = true;
    } else {
      REXGPU_ERROR("Failed to dump the first matching texture load");
    }
  }

  texture.LogAction("Loaded");

  return true;
}

void TextureCache::OnTextureLoadFailure(LoadFailureSite site, const Texture& texture,
                                        bool load_base, bool load_mips) {
  LoadFailureState& failures = load_failure_state;
  if (failures.retry) {
    // Unchanged fetch registers keep the failed binding marked in sync, so
    // without this the following draws sample stale contents until BeginFrame.
    texture_became_outdated_.store(true, std::memory_order_release);
  }
  if (!failures.log) {
    return;
  }
  ++failures.total;
  failures.last_submission = current_submission_index_;
  if (++failures.frame_count > kMaxLoadFailureLogsPerFrame) {
    return;
  }
  const TextureKey& key = texture.key();
  const char* site_name = "unknown";
  switch (site) {
    case LoadFailureSite::kPrepare:
      site_name = "prepare";
      break;
    case LoadFailureSite::kBatchRequest:
      site_name = "batch_request";
      break;
    case LoadFailureSite::kRetry:
      site_name = "retry";
      break;
    case LoadFailureSite::kDirect:
      site_name = "direct";
      break;
  }
  REXGPU_WARN(
      "Texture load failed: frame={} submission={} site={} base=0x{:08X} mip=0x{:08X} format={} "
      "size={}x{}x{} load_base={} load_mips={} outdated=0x{:X}",
      failures.frame_index, current_submission_index_, site_name, uint32_t(key.base_page) << 12,
      uint32_t(key.mip_page) << 12, int(key.format), key.GetWidth(), key.GetHeight(),
      key.GetDepthOrArraySize(), load_base ? 1 : 0, load_mips ? 1 : 0, texture.outdated_mask());
}

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
  const auto& regs = register_file();

  if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {
    // A texture has become outdated - make sure whether textures are outdated
    // is rechecked in this draw and in subsequent ones to reload the new data
    // if needed.
    ResetTextureBindings();
  }

  // Update the texture keys and the textures.
  uint32_t bindings_changed = 0;
  std::vector<PendingTextureLoad> pending_texture_loads;
  std::vector<PendingSharedMemoryRange> pending_shared_memory_ranges;
  auto queue_pending_texture_load = [&](Texture* texture) {
    if (texture == nullptr) {
      return;
    }
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture == texture) {
        return;
      }
    }
    PendingTextureLoad pending_load;
    PendingSharedMemoryRange pending_ranges[2];
    size_t pending_range_count = 0;
    if (!PrepareTextureLoad(*texture, pending_load, pending_ranges, pending_range_count)) {
      return;
    }
    pending_texture_loads.push_back(pending_load);
    for (size_t i = 0; i < pending_range_count; ++i) {
      pending_shared_memory_ranges.push_back(pending_ranges[i]);
    }
  };
  uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  uint32_t index = 0;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    uint32_t index_bit = UINT32_C(1) << index;
    textures_remaining &= ~index_bit;
    TextureBinding& binding = texture_bindings_[index];
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
    TextureKey old_key = binding.key;
    uint8_t old_swizzled_signs = binding.swizzled_signs;
    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
    texture_bindings_in_sync_ |= index_bit;
    if (!binding.key.is_valid) {
      if (old_key.is_valid) {
        bindings_changed |= index_bit;
      }
      binding.Reset();
      continue;
    }
    uint32_t old_host_swizzle = binding.host_swizzle;
    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));

    // Check if need to load the unsigned and the signed versions of the texture
    // (if the format is emulated with different host bit representations for
    // signed and unsigned - otherwise only the unsigned one is loaded).
    bool key_changed = binding.key != old_key;
    bool any_sign_was_not_signed = texture_util::IsAnySignNotSigned(old_swizzled_signs);
    bool any_sign_was_signed = texture_util::IsAnySignSigned(old_swizzled_signs);
    bool any_sign_is_not_signed = texture_util::IsAnySignNotSigned(binding.swizzled_signs);
    bool any_sign_is_signed = texture_util::IsAnySignSigned(binding.swizzled_signs);
    if (key_changed || binding.host_swizzle != old_host_swizzle ||
        any_sign_is_not_signed != any_sign_was_not_signed ||
        any_sign_is_signed != any_sign_was_signed) {
      bindings_changed |= index_bit;
    }
    bool load_unsigned_data = false, load_signed_data = false;
    if (IsSignedVersionSeparateForFormat(binding.key)) {
      // Can reuse previously loaded unsigned/signed versions if the key is the
      // same and the texture was previously bound as unsigned/signed
      // respectively (checking the previous values of signedness rather than
      // binding.texture != nullptr and binding.texture_signed != nullptr also
      // prevents repeated attempts to load the texture if it has failed to
      // load).
      if (any_sign_is_not_signed) {
        if (key_changed || !any_sign_was_not_signed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        }
      } else {
        binding.texture = nullptr;
      }
      if (any_sign_is_signed) {
        if (key_changed || !any_sign_was_signed) {
          TextureKey signed_key = binding.key;
          signed_key.signed_separate = 1;
          binding.texture_signed = FindOrCreateTexture(signed_key);
          load_signed_data = true;
        }
      } else {
        binding.texture_signed = nullptr;
      }
    } else {
      // Same resource for both unsigned and signed, but descriptor formats may
      // be different.
      if (key_changed) {
        binding.texture = FindOrCreateTexture(binding.key);
        load_unsigned_data = true;
      }
      binding.texture_signed = nullptr;
    }
    if (load_unsigned_data) {
      queue_pending_texture_load(binding.texture);
    }
    if (load_signed_data) {
      queue_pending_texture_load(binding.texture_signed);
    }
  }

  COUNT_profile_set("gpu/texture_cache/request_textures_pending_load_count",
                    uint32_t(pending_texture_loads.size()));
  COUNT_profile_set("gpu/texture_cache/request_textures_pending_range_count",
                    uint32_t(pending_shared_memory_ranges.size()));

  bool batched_shared_memory_request_succeeded = true;
  std::vector<std::pair<uint32_t, uint32_t>> pending_shared_memory_range_pairs;
  if (!pending_shared_memory_ranges.empty()) {
    pending_shared_memory_range_pairs.reserve(pending_shared_memory_ranges.size());
    for (const PendingSharedMemoryRange& pending_range : pending_shared_memory_ranges) {
      pending_shared_memory_range_pairs.emplace_back(pending_range.start, pending_range.length);
    }
    batched_shared_memory_request_succeeded = shared_memory().RequestRanges(
        pending_shared_memory_range_pairs.data(), pending_shared_memory_range_pairs.size());
  }

  if (batched_shared_memory_request_succeeded) {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      CommitPreparedTextureLoad(pending_load);
    }
  } else {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
        const uint64_t fixture_entry_generation = pending_load.texture->content_generation();
#endif
        pending_load.texture->RestoreOutdatedAfterLoadFailure(
            global_critical_region_.Acquire(), pending_load.load_base, pending_load.load_mips);
        OnTextureLoadFailure(LoadFailureSite::kBatchRequest, *pending_load.texture,
                             pending_load.load_base, pending_load.load_mips);
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
        texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kBatchRequestFailure,
                                      *pending_load.texture, pending_load.load_base,
                                      pending_load.load_mips, fixture_entry_generation);
#endif
      }
    }
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
        LoadTextureDataForSite(*pending_load.texture, LoadFailureSite::kRetry);
      }
    }
  }
  if (bindings_changed) {
    UpdateTextureBindingsImpl(bindings_changed);
  }
}

const char* TextureCache::TextureKey::GetLogDimensionName(xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      assert_unhandled_case(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
      action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "", GetWidth(), GetHeight(),
      GetDepthOrArraySize(), GetLogDimensionName(), FormatInfo::Get(format)->name,
      mip_max_level + 1, packed_mips ? "" : "un", mip_max_level != 0 ? "s" : "", base_page << 12,
      pitch << 5, mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
      "0x{:08X})",
      action, key_.tiled ? "tiled" : "linear", key_.scaled_resolve ? "scaled " : "",
      key_.GetWidth(), key_.GetHeight(), key_.GetDepthOrArraySize(), key_.GetLogDimensionName(),
      FormatInfo::Get(key_.format)->name, key_.mip_max_level + 1, key_.packed_mips ? "" : "un",
      key_.mip_max_level != 0 ? "s" : "", key_.base_page << 12, key_.pitch << 5, GetGuestBaseSize(),
      key_.mip_page << 12, GetGuestMipsSize());
}

// The texture must be in the recent usage list. Place it in front now because
// after creation, the texture will likely be used immediately, and it should
// not be destroyed immediately after creation if dropping of old textures is
// performed somehow. The list is maintained by the Texture, not the
// TextureCache itself (unlike the `textures_` container).
TextureCache::Texture::Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage),
      force_load_3d_tiling_(false),
      instance_id_(next_texture_instance_id.fetch_add(1, std::memory_order_relaxed)) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  // Never try to upload data that doesn't exist.
  base_outdated_ = guest_layout().base.level_data_extent_bytes != 0;
  mips_outdated_ = guest_layout().mips_total_extent_bytes != 0;
  outdated_mask_.store(
      (base_outdated_ ? kOutdatedBitBase : 0) | (mips_outdated_ ? kOutdatedBitMips : 0),
      std::memory_order_relaxed);
}

TextureCache::Texture::~Texture() {
  if (mips_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(base_watch_handle_);
  }

  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

diagnostic::DrawCaptureGuestMemoryVersion TextureCache::GetActiveTextureGuestMemoryVersion(
    uint32_t fetch_constant_index, bool is_signed) const {
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    return {};
  }
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding || binding->key.scaled_resolve) {
    return {};
  }
  const Texture* texture = is_signed && IsSignedVersionSeparateForFormat(binding->key)
                               ? binding->texture_signed
                               : binding->texture;
  return texture ? texture->GetGuestMemoryVersion() : diagnostic::DrawCaptureGuestMemoryVersion{};
}

diagnostic::DrawCaptureGuestMemoryVersion TextureCache::Texture::GetGuestMemoryVersion() const {
  if (key().scaled_resolve || force_load_3d_tiling()) {
    return {};
  }
  diagnostic::DrawCaptureGuestMemoryVersion version;
  version.identity = instance_id();
  version.generation = content_generation();
  version.base_address = key().base_page << 12;
  version.base_size = GetGuestBaseSize();
  version.mip_address = key().mip_page << 12;
  version.mip_size = GetGuestMipsSize();
  version.context = this;
  version.check = [](const void* context, uint64_t generation) noexcept {
    const auto& texture = *static_cast<const Texture*>(context);
    // An invalidated watch is not rearmed until upload. Repeated writes in
    // that interval need not increment the generation, so also check its mask.
    return texture.content_generation() == generation && !texture.outdated_mask() &&
           texture.content_generation() == generation;
  };
  return version;
}

void TextureCache::Texture::MakeUpToDateAndWatch(
    [[maybe_unused]] const std::unique_lock<std::recursive_mutex>& global_lock, bool base,
    bool mips) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (base && base_outdated_) {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = false;
    base_watch_handle_ = shared_memory.WatchMemoryRange(
        key().base_page << 12, GetGuestBaseSize(), TextureCache::WatchCallback, this, nullptr, 0);
    outdated_mask_.fetch_and(~kOutdatedBitBase, std::memory_order_release);
  }
  if (mips && mips_outdated_) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = false;
    mips_watch_handle_ = shared_memory.WatchMemoryRange(
        key().mip_page << 12, GetGuestMipsSize(), TextureCache::WatchCallback, this, nullptr, 1);
    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);
  }
}

void TextureCache::Texture::RestoreOutdatedAfterLoadFailure(
    [[maybe_unused]] const std::unique_lock<std::recursive_mutex>& global_lock, bool base,
    bool mips) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  uint32_t restored_mask = 0;
  if (base) {
    if (base_watch_handle_) {
      shared_memory.UnwatchMemoryRange(base_watch_handle_);
      base_watch_handle_ = nullptr;
    }
    base_outdated_ = true;
    restored_mask |= kOutdatedBitBase;
  }
  if (mips) {
    if (mips_watch_handle_) {
      shared_memory.UnwatchMemoryRange(mips_watch_handle_);
      mips_watch_handle_ = nullptr;
    }
    mips_outdated_ = true;
    restored_mask |= kOutdatedBitMips;
  }
  outdated_mask_.fetch_or(restored_mask, std::memory_order_release);
  // Removing a watch allows writes with no callback. Retire tokens before a
  // retry rearms this instance. Publish the dirty mask before the new version.
  if (restored_mask) {
    content_generation_.fetch_add(1, std::memory_order_acq_rel);
  }
}

void TextureCache::Texture::MarkAsUsed() {
  if (!in_usage_list_) {
    return;
  }
  assert_true(last_usage_submission_index_ <= texture_cache_.current_submission_index_);
  // This is called very frequently, don't relink unless needed for caching.
  if (last_usage_submission_index_ >= texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    // Already the most recently used.
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

void TextureCache::Texture::WatchCallback(
    [[maybe_unused]] const std::unique_lock<std::recursive_mutex>& global_lock, bool is_mip) {
  if (is_mip) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitMips, std::memory_order_release);
  } else {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitBase, std::memory_order_release);
  }
  // A reader observing this generation must also observe the dirty mask.
  content_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  texture.WatchCallback(global_lock, argument != 0);
  texture.texture_cache().texture_became_outdated_.store(true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  COUNT_profile_set("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  // Check if the texture is a scaled resolve texture.
  if (IsDrawResolutionScaled() && key.tiled && IsScaledResolveSupportedForFormat(key)) {
    texture_util::TextureGuestLayout scaled_resolve_guest_layout = key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(key.base_page << 12,
                               scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(key.mip_page << 12,
                               scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  auto get_host_extent = [this, &key](uint32_t& host_width_out, uint32_t& host_height_out,
                                      uint32_t& host_depth_or_array_size_out) {
    host_width_out = key.GetWidth();
    host_height_out = key.GetHeight();
    if (key.scaled_resolve) {
      host_width_out *= draw_resolution_scale_x();
      host_height_out *= draw_resolution_scale_y();
    }
    host_depth_or_array_size_out = key.GetDepthOrArraySize();
  };
  auto try_drop_top_level = [&key]() {
    if (!key.mip_page || !key.mip_max_level) {
      return false;
    }
    uint32_t old_width = key.GetWidth();
    uint32_t old_height = key.GetHeight();
    uint32_t old_depth_or_array_size = key.GetDepthOrArraySize();
    uint32_t new_width = std::max(old_width >> 1, UINT32_C(1));
    uint32_t new_height = std::max(old_height >> 1, UINT32_C(1));
    uint32_t new_depth_or_array_size = key.dimension == xenos::DataDimension::k3D
                                           ? std::max(old_depth_or_array_size >> 1, UINT32_C(1))
                                           : old_depth_or_array_size;
    if (new_width == old_width && new_height == old_height &&
        new_depth_or_array_size == old_depth_or_array_size) {
      return false;
    }
    const FormatInfo* format_info = FormatInfo::Get(key.format);
    uint32_t rebased_pitch_texels = std::max(rex::next_pow2(old_width) >> 1, UINT32_C(1));
    rebased_pitch_texels = rex::align(rebased_pitch_texels, format_info->block_width);
    rebased_pitch_texels = rex::align(rebased_pitch_texels, xenos::kTextureTileWidthHeight);
    if (!key.tiled) {
      uint32_t rebased_pitch_blocks = rebased_pitch_texels / format_info->block_width;
      uint32_t rebased_row_pitch_bytes = rebased_pitch_blocks * format_info->bytes_per_block();
      rebased_row_pitch_bytes =
          rex::align(rebased_row_pitch_bytes, xenos::kTextureLinearRowAlignmentBytes);
      rebased_pitch_blocks = rebased_row_pitch_bytes / format_info->bytes_per_block();
      rebased_pitch_texels = rebased_pitch_blocks * format_info->block_width;
    }
    uint32_t rebased_pitch = std::max(rebased_pitch_texels >> 5, UINT32_C(1));
    if (rebased_pitch > ((UINT32_C(1) << 9) - 1)) {
      return false;
    }
    key.width_minus_1 = new_width - 1;
    key.height_minus_1 = new_height - 1;
    key.depth_or_array_size_minus_1 = new_depth_or_array_size - 1;
    key.base_page = key.mip_page;
    key.mip_page = 0;
    key.pitch = rebased_pitch;
    key.mip_max_level = 0;
    key.packed_mips = 0;
    return true;
  };
  uint32_t host_width, host_height, host_depth_or_array_size;
  get_host_extent(host_width, host_height, host_depth_or_array_size);
  // If the host can't support the full texture extent, first try using the
  // unscaled version of a scaled resolve texture, then fall back to the first
  // stored mip level only.
  uint32_t max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  uint32_t max_host_depth_or_array_size = GetMaxHostTextureDepthOrArraySize(key.dimension);
  while (true) {
    bool width_height_too_large =
        host_width > max_host_width_height || host_height > max_host_width_height;
    bool depth_or_array_too_large = host_depth_or_array_size > max_host_depth_or_array_size;
    if (!width_height_too_large && !depth_or_array_too_large) {
      break;
    }
    bool size_adjusted = false;
    if (key.scaled_resolve) {
      key.scaled_resolve = 0;
      size_adjusted = true;
    } else if ((width_height_too_large ||
                (depth_or_array_too_large && key.dimension == xenos::DataDimension::k3D)) &&
               try_drop_top_level()) {
      size_adjusted = true;
    }
    if (!size_adjusted) {
      return nullptr;
    }
    get_host_extent(host_width, host_height, host_depth_or_array_size);
  }

  // Try to find an existing texture.
  // TODO(Triang3l): Reuse a texture with mip_page unchanged, but base_page
  // previously 0, now not 0, to save memory - common case in streaming.
  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  // Create the texture and add it to the map.
  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      return nullptr;
    }
    assert_true(new_texture->key() == key);
    texture = textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  return texture;
}

bool TextureCache::LoadTextureData(Texture& texture) {
  return LoadTextureDataForSite(texture, LoadFailureSite::kDirect);
}

bool TextureCache::LoadTextureDataForSite(Texture& texture, LoadFailureSite site) {
  PendingTextureLoad pending_load;
  PendingSharedMemoryRange pending_ranges[2];
  size_t pending_range_count = 0;
  if (!PrepareTextureLoad(texture, pending_load, pending_ranges, pending_range_count)) {
    return true;
  }

  std::pair<uint32_t, uint32_t> pending_range_pairs[2];
  for (size_t i = 0; i < pending_range_count; ++i) {
    pending_range_pairs[i] = std::make_pair(pending_ranges[i].start, pending_ranges[i].length);
  }
  if (!shared_memory().RequestRanges(pending_range_pairs, pending_range_count)) {
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
    const uint64_t fixture_entry_generation = texture.content_generation();
#endif
    texture.RestoreOutdatedAfterLoadFailure(global_critical_region_.Acquire(),
                                            pending_load.load_base, pending_load.load_mips);
    OnTextureLoadFailure(site, texture, pending_load.load_base, pending_load.load_mips);
#if defined(REX_INTERNAL_D3D12_TEXTURE_FIXTURE)
    texture_fixture::TraceTexture(this, texture_fixture::TraceKind::kDirectRequestFailure, texture,
                                  pending_load.load_base, pending_load.load_mips,
                                  fixture_entry_generation);
#endif
    return false;
  }

  return CommitPreparedTextureLoad(pending_load);
}

void TextureCache::BindingInfoFromFetchConstant(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                TextureKey& key_out, uint8_t* swizzled_signs_out) {
  // Reset the key and the signedness.
  key_out.MakeInvalid();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
        break;
      }
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "has \"invalid\" type! This is incorrect behavior, but you can try "
          "bypassing this by launching Xenia with "
          "--gpu_allow_invalid_fetch_constants=true.",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
    default:
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "is completely invalid!",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
  }

  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    // No texture data at all.
    return;
  }
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    // TODO(Triang3l): Support long 1D textures.
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      REXGPU_ERROR(
          "1D texture is too wide ({}) - ignoring! Report the game to Xenia "
          "developers",
          width_minus_1 + 1);
      is_invalid_1d = true;
    }
    assert_false(fetch.tiled);
    if (fetch.tiled) {
      REXGPU_ERROR(
          "1D texture has tiling enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    assert_false(fetch.packed_mips);
    if (fetch.packed_mips) {
      REXGPU_ERROR(
          "1D texture has packed mips enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = fetch.pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = texture_util::SwizzleSigns(fetch);
  }
}

bool TextureCache::PrepareFrameInputTexture(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    diagnostic::DrawCaptureFrameInputTextureView& view) const {
  view.native_source_generation = 0;
  view.available = false;
  view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureSource;
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return false;
  }
  const bool selected_cube = IsPreparedCubeFetch(fetch);
  if ((fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked) && !selected_cube) {
    view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureDimension;
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(fetch.format);
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (!format_info->block_width || !format_info->block_height || !bytes_per_block ||
      (bytes_per_block & (bytes_per_block - 1))) {
    view.unavailable_reason = diagnostic::DrawCaptureReason::kTextureFormat;
    return false;
  }
  // Validate before the legacy assert-based metadata helpers. Mip-only and
  // malformed pitch/LOD layouts are outside this CPU preparation subset.
  const uint32_t width = fetch.size_2d.width + 1;
  const uint32_t height = fetch.size_2d.height + 1;
  if (!fetch.base_address || !fetch.pitch || (fetch.pitch << 5) < width ||
      fetch.mip_min_level != 0 || fetch.mip_max_level > rex::log2_floor(std::max(width, height))) {
    return false;
  }
  TextureKey key;
  uint8_t signs;
  BindingInfoFromFetchConstant(fetch, key, &signs);
  if (!key.is_valid) {
    return false;
  }
  TextureInfo info;
  if (!TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  const auto layout = key.GetGuestLayout();
  // TextureInfo can omit bytes from a source-backed Prepared range. Expose
  // canonical layout only for selected forms; leave ordinary cache metadata and
  // other undercovered forms unchanged.
  const bool selected_signed_rg16 = IsPreparedSignedRg16Float(fetch, view, signs);
  const bool selected_tiled_bc = IsPreparedUnsignedTiledBc(fetch, view, signs);
  const bool selected_unsigned_bc1 = IsPreparedUnsignedBc1Packed(fetch, view, signs);
  const bool selected_unsigned_bc2 = IsPreparedUnsignedBc2Packed(fetch, view, signs);
  const bool selected_unsigned_bc3 = IsPreparedUnsignedBc3Packed(fetch, view, signs);
  const bool selected_unsigned_rgba8_r8 = IsPreparedUnsignedRgba8R8Packed(fetch, view, signs);
  if (key.base_page && info.memory.base_address != key.base_page << 12) {
    return false;
  }
  if (layout.mips_total_extent_bytes) {
    if (info.memory.mip_address != key.mip_page << 12) {
      return false;
    }
    if (info.memory.mip_size < layout.mips_total_extent_bytes) {
      if (!selected_unsigned_bc1 && !selected_unsigned_bc2 && !selected_unsigned_bc3 &&
          !selected_unsigned_rgba8_r8 && !selected_cube && !selected_tiled_bc) {
        return false;
      }
      info.memory.mip_size = layout.mips_total_extent_bytes;
    }
  }
  if (key.base_page && info.memory.base_size < layout.base.level_data_extent_bytes) {
    if ((!selected_signed_rg16 && !selected_unsigned_rgba8_r8 && !selected_tiled_bc) ||
        !layout.base.level_data_extent_bytes) {
      return false;
    }
    info.memory.base_size = layout.base.level_data_extent_bytes;
  }
  const auto range_valid = [](uint32_t address, uint32_t size) {
    return address <= SharedMemory::kBufferSize &&
           uint64_t(size) <= uint64_t(SharedMemory::kBufferSize) - address;
  };
  if ((!info.memory.base_size && !info.memory.mip_size) ||
      !range_valid(info.memory.base_address, info.memory.base_size) ||
      !range_valid(info.memory.mip_address, info.memory.mip_size)) {
    return false;
  }
  view.format = static_cast<uint32_t>(info.format);
  view.endianness = static_cast<uint32_t>(info.endianness);
  view.dimension = static_cast<uint32_t>(info.dimension);
  view.width = info.width;
  view.height = info.height;
  view.depth = info.depth;
  view.pitch = info.pitch;
  view.mip_min_level = info.mip_min_level;
  view.mip_max_level = info.mip_max_level;
  view.tiled = info.is_tiled;
  view.packed_mips = info.has_packed_mips;
  view.base_address = info.memory.base_address;
  view.base_size = info.memory.base_size;
  view.mip_address = info.memory.mip_address;
  view.mip_size = info.memory.mip_size;
  view.swizzled_signs = signs;
  view.component_mapping = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  view.required = view.is_signed ? texture_util::IsAnySignSigned(signs)
                                 : texture_util::IsAnySignNotSigned(signs);
  view.available = true;
  view.unavailable_reason = diagnostic::DrawCaptureReason::kNone;
  return true;
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  uint32_t bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(uint64_t add, uint64_t subtract) {
  textures_total_host_memory_usage_ = textures_total_host_memory_usage_ - subtract + add;
  COUNT_profile_set(
      "gpu/texture_cache/total_host_memory_usage_mb",
      uint32_t((textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20));
}

bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled, uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  // Two-level check for faster rejection since resolve targets are usually
  // placed in relatively small and localized memory portions (confirmed by
  // testing - pretty much all times the deeper level was entered, the texture
  // was a resolve target).
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  uint32_t block_first = page_first >> 5;
  uint32_t block_last = page_last >> 5;
  uint32_t l2_block_first = block_first >> 6;
  uint32_t l2_block_last = block_last >> 6;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = l2_block_first; i <= l2_block_last; ++i) {
    uint64_t l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    uint32_t block_relative_index;
    while (rex::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      uint32_t block_index = (i << 6) + block_relative_index;
      uint32_t check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(
    const std::unique_lock<std::recursive_mutex>& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(global_lock, address_first, address_last,
                                                  invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(
    const std::unique_lock<std::recursive_mutex>& global_lock, uint32_t address_first,
    uint32_t address_last, bool invalidated_by_gpu) {
  assert_true(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // Resolves themselves do exactly the opposite of what this should do.
    return;
  }
  // Mark scaled resolve ranges as non-scaled. Textures themselves will be
  // invalidated by their shared memory watches.
  uint32_t resolve_page_first = address_first >> 12;
  uint32_t resolve_page_last = address_last >> 12;
  uint32_t resolve_block_first = resolve_page_first >> 5;
  uint32_t resolve_block_last = resolve_page_last >> 5;
  uint32_t resolve_l2_block_first = resolve_block_first >> 6;
  uint32_t resolve_l2_block_last = resolve_block_last >> 6;
  for (uint32_t i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    uint64_t resolve_l2_block = scaled_resolve_pages_l2_[i];
    if (REXCVAR_GET(pre_mask_resolve_l2_block)) {
      // Pre-mask to only process blocks within the write range.
      if (i == resolve_l2_block_first) {
        resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
      }
      if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
        resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
      }
    }
    uint32_t resolve_block_relative_index;
    while (rex::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      uint32_t resolve_block_index = (i << 6) + resolve_block_relative_index;
      uint32_t resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last && (resolve_page_last & 31) != 31) {
        resolve_keep_bits |= ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &= ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
}

}  // namespace rex::graphics
