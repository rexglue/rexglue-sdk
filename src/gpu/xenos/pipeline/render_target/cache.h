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

#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <format>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register_file.h>
#include <rex/xenos/registers.h>
#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/draw_extent_estimator.h"
#include <rex/xenos/xenos.h>
#include <rex/types.h>

namespace rex::gpu {

class RenderTargetCache {
 public:
  /** Emulation implementation path for the output merger. */
  enum class Path : u8 {
    /**
     * Approximate method using conventional host render targets, copying ("transferring ownership"
     * of tiles) between them to support aliasing. May be irreparably inaccurate, at the mercy of
     * the host fixed function output merger, mainly because it must perform blending, and a
     * different pixel format behaves differently (the range is clamped for normalized formats but
     * not for floating point ones).
     *
     * On a Direct3D 11 level device 8_8_8_8, 2_10_10_10, 32_FLOAT, 32_32_FLOAT and D24S8 map
     * directly, disregarding blending internal precision. D24FS8 maps directly with a truncated
     * SV_DepthLessEqual output (SV_Depth is suboptimal as it prevents early depth and stencil). For
     * bit exact reinterpretation to and from D24F in unmodified areas using pixel shader depth
     * output without unrestricted depth range, guest depth 0...1 maps to 0...0.5 on the host in the
     * viewport and conversion. 16_16_FLOAT and 16_16_16_16_FLOAT map directly except for rare edge
     * cases, as the Xenos float16 has no special values.
     *
     * Formats with significant differences are as follows. 8_8_8_8_GAMMA uses a piecewise linear
     * gamma curve different from sRGB, so it needs either shader conversion (incorrect blending) or
     * sRGB render targets with conversion on resolve or reading the resolved data as an sRGB
     * texture (incorrect when the guest reads the data itself). 2_10_10_10_FLOAT has ranges
     * significantly different from float16, with a much smaller RGB range and a fixed point alpha
     * of only 2 bits. 16_16 and 16_16_16_16 have a -32 to 32 range rather than -1 to 1, so they
     * need either range truncation for correct blending or division by 32 in shaders, which breaks
     * multiplication in blending.
     */
    kHostRenderTargets,

    /**
     * Custom output merger with full per pixel and per sample control, available only on hosts with
     * raster ordered writes from pixel shaders.
     */
    kPixelShaderInterlock,
  };

  /** Flags appended to the format in the format constant via bitwise OR. */
  enum : u8 {
    kPSIColorFormatFlag64bppShift = xenos::kColorRenderTargetFormatBits,
    /** Requires clamping of blending sources and factors. */
    kPSIColorFormatFlagFixedPointColorShift,
    kPSIColorFormatFlagFixedPointAlphaShift,

    kPSIColorFormatFlag64bpp = static_cast<u32>(1) << kPSIColorFormatFlag64bppShift,
    kPSIColorFormatFlagFixedPointColor = static_cast<u32>(1)
                                         << kPSIColorFormatFlagFixedPointColorShift,
    kPSIColorFormatFlagFixedPointAlpha = static_cast<u32>(1)
                                         << kPSIColorFormatFlagFixedPointAlphaShift,
  };

  static constexpr u32 AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat format) {
    u32 format_flags = static_cast<u32>(format);
    if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
        format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
      format_flags |= kPSIColorFormatFlag64bpp;
    }
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
        format == xenos::ColorRenderTargetFormat::k_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
      format_flags |= kPSIColorFormatFlagFixedPointColor | kPSIColorFormatFlagFixedPointAlpha;
    } else if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) {
      format_flags |= kPSIColorFormatFlagFixedPointAlpha;
    }
    return format_flags;
  }

  /**
   * Returns the blending clamp ranges and the keep masks of the bits left unwritten by write_mask.
   * All keep bits are set for an empty write mask or an invalid format, which makes the shaders
   * skip the render target. For k_16_16 and k_16_16_16_16 the alpha clamp is nonzero since alpha
   * clamping affects the blending source (the render target is fixed point), and NaN should become
   * 0 rather than the lowest negative number as the Direct3D 11.3 functional specification defines
   * SNorm conversion, which needs separate handling. The float16 formats have no NaNs on the GPU,
   * but the extended range cannot be used with Direct3D and Vulkan conversions. The 32 bit float
   * formats use NaN clamps, so min and max pick the original value.
   *
   * @param format Color render target format.
   * @param write_mask Color component write mask.
   * @param clamp_rgb_low Receives the lower clamp for RGB.
   * @param clamp_alpha_low Receives the lower clamp for alpha.
   * @param clamp_rgb_high Receives the upper clamp for RGB.
   * @param clamp_alpha_high Receives the upper clamp for alpha.
   * @param keep_mask_low Receives the keep mask of the low 32 bits.
   * @param keep_mask_high Receives the keep mask of the high 32 bits.
   */
  static void GetPSIColorFormatInfo(xenos::ColorRenderTargetFormat format, u32 write_mask,
                                    f32& clamp_rgb_low, f32& clamp_alpha_low, f32& clamp_rgb_high,
                                    f32& clamp_alpha_high, u32& keep_mask_low, u32& keep_mask_high);

  virtual ~RenderTargetCache();

  virtual Path GetPath() const = 0;

  /**
   * Resolution scaling on the EDRAM side multiplies the EDRAM tile size by the resolution scale.
   * Only integer scales are supported, since fractional ones such as 1.5 break pixel coverage of
   * odd sized screen space geometry (notably the 1x1 final step of reduction passes), break the gap
   * fill used for 2x and 3x scaling when the half pixel offset becomes whole pixel, and complicate
   * rounding of integer coordinates.
   */
  u32 GetDrawResolutionScaleX() const { return draw_resolution_scale_x_; }
  u32 GetDrawResolutionScaleY() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return GetDrawResolutionScaleX() > 1 || GetDrawResolutionScaleY() > 1;
  }

  /**
   * Virtual so both the common code and the implementation can act. Must not be called from
   * destructors, as it does work shutdown does not need.
   */
  virtual void ClearCache();

  virtual void BeginFrame();

  /**
   * Updates the bound render targets and EDRAM ownership for a draw.
   *
   * Render targets whose EDRAM base conflicts with another are eliminated, since binding one render
   * target into multiple slots is an error in most host implementations and behaves unpredictably.
   * Depth is least important as it is earlier in the pipeline, and lower indexed render targets are
   * more important than higher ones, because a shader may select between 2 render targets to write
   * dynamically (through a static bool constant condition) while the other state implies the same
   * render target bound twice. On Direct3D 9 a color output not written on the taken control flow
   * leaves the render target unwritten, while Direct3D 10 treats a declared output as always
   * written, with an undefined value otherwise.
   *
   * Render targets are sorted by EDRAM base and then by pipeline order. A render target placed
   * closer to the end of the EDRAM truncates the previous one, and for the same base color is more
   * important than depth, which may be a needless leftover and is tested before color is written.
   * Estimated EDRAM ranges of render targets normally intersect when drawing without a viewport,
   * since nothing clamps the estimated height, and with multiple render targets bound. Distances
   * between bases are compared as if 64bpp (32bpp contributions doubled and clamping for 32bpp
   * halved), because 32bpp render targets can be combined with 64bpp ones twice as long.
   *
   * @param is_rasterization_done True when the draw rasterizes.
   * @param normalized_depth_control Depth control with the register normalization applied.
   * @param normalized_color_mask Color write mask, 4 bits per render target.
   * @param vertex_shader Vertex shader of the draw, used to estimate the extent.
   * @return False when a required render target could not be created or the surface is unsupported.
   */
  virtual bool Update(bool is_rasterization_done,
                      xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                      u32 normalized_color_mask, const Shader& vertex_shader);

  /**
   * Returns a mask where bit 0 is set when a depth render target is bound on the host and
   * bits 1 and up are set for bound color render targets.
   *
   * @param depth_and_color_formats_out Receives each target's resource format, may be null.
   * @return Bit mask of the bound render targets.
   */
  u32 GetLastUpdateBoundRenderTargets(u32* depth_and_color_formats_out = nullptr) const;

 protected:
  RenderTargetCache(const xenos::RegisterFile& register_file, const runtime::Memory& memory,
                    u32 draw_resolution_scale_x, u32 draw_resolution_scale_y)
      : register_file_(register_file),
        draw_resolution_scale_x_(draw_resolution_scale_x),
        draw_resolution_scale_y_(draw_resolution_scale_y),
        draw_extent_estimator_(register_file, memory) {
    REX_ASSERT(draw_resolution_scale_x != 0);
    REX_ASSERT(draw_resolution_scale_y != 0);
  }

  const xenos::RegisterFile& GetRegisterFile() const { return register_file_; }

  virtual bool IsGammaFormatHostStorageSeparate() const = 0;

  /** Call last in implementation initialization, once things like the path are initialized. */
  void InitializeCommon();
  /**
   * May be called from the destructor or from the implementation shutdown to destroy all render
   * targets before destroying what they depend on in the implementation.
   */
  void DestroyAllRenderTargets() noexcept;
  /** Call last in implementation shutdown, also callable from the destructor. */
  void ShutdownCommon() noexcept;

  union RenderTargetKey {
    u32 key;
    struct {
      u32 base_tiles : xenos::kEDRAMBaseTilesBits;
      /**
       * At most align(8192 * 2, 80) / 80 = 205 at 4x MSAA (2 horizontal samples). Doubled at 64bpp,
       * or use GetPitchTiles.
       */
      u32 pitch_tiles_at_32bpp : 8;
      xenos::MSAASamples msaa_samples : xenos::kMSAASamplesBits;
      u32 is_depth : 1;
      u32 resource_format
          : xenos::kRenderTargetFormatBits; /**< Ignores blending precision and sRGB. */
    };

    RenderTargetKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const RenderTargetKey& render_target_key) const {
        return std::hash<u32>{}(render_target_key.key);
      }
    };
    bool operator==(const RenderTargetKey& other_key) const { return key == other_key.key; }
    bool operator!=(const RenderTargetKey& other_key) const { return !(*this == other_key); }

    bool IsEmpty() const {
      // Meaningless when pitch_tiles_at_32bpp is 0, so only an all zero key counts as empty.
      return key == 0;
    }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      REX_ASSERT(!is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      REX_ASSERT(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
    bool Is64bpp() const {
      if (is_depth) {
        return false;
      }
      return xenos::IsColorRenderTargetFormat64bpp(GetColorFormat());
    }
    const char* GetFormatName() const {
      return is_depth ? xenos::GetDepthRenderTargetFormatName(GetDepthFormat())
                      : xenos::GetColorRenderTargetFormatName(GetColorFormat());
    }

    u32 GetPitchTiles() const { return pitch_tiles_at_32bpp << u32(Is64bpp()); }
    static constexpr u32 GetWidth(u32 pitch_tiles_at_32bpp, xenos::MSAASamples msaa_samples) {
      return pitch_tiles_at_32bpp * (xenos::kEDRAMTileWidthSamples >>
                                     static_cast<u32>(msaa_samples >= xenos::MSAASamples::k4X));
    }
    u32 GetWidth() const { return GetWidth(pitch_tiles_at_32bpp, msaa_samples); }

    std::string GetDebugName() const {
      return std::format("RT @ {}t, <{}t>, {}xMSAA, {}", base_tiles, GetPitchTiles(),
                         static_cast<u32>(1) << u32(msaa_samples), GetFormatName());
    }
  };

  class RenderTarget : public rex::NonCopyable {
   public:
    virtual ~RenderTarget() = default;
    RenderTargetKey GetKey() const { return key_; }

   protected:
    RenderTarget(RenderTargetKey key) : key_(key) {}

   private:
    RenderTargetKey key_;
  };

  /**
   * Range of EDRAM 80x16 sample tiles whose ownership moves between host render targets. The data
   * is copied bit exactly from the guest perspective, by drawing rectangles with a pixel shader
   * that converts the previous host render target to a guest bit pattern and reinterprets it in the
   * new format (through an integer view of the texture when dangerous, such as float16 with non
   * propagated NaN). If depth is emulated with float32, data may be lost, so for depth both the
   * guest format ownership and the float32 ownership are tracked, and to let color data overwrite
   * depth data, loading during the transfer uses intersections of the guest ownership ranges and
   * the float32 ownership ranges. A transfer happens when a render target is needed, based on the
   * current viewport, or, if no viewport is available, ownership of the rest of the EDRAM is
   * transferred.
   */
  struct Transfer {
    u32 start_tiles;
    u32 end_tiles;
    RenderTarget* source;
    RenderTarget* host_depth_source;
    Transfer(u32 start_tiles, u32 end_tiles, RenderTarget* source, RenderTarget* host_depth_source)
        : start_tiles(start_tiles),
          end_tiles(end_tiles),
          source(source),
          host_depth_source(host_depth_source) {
      REX_ASSERT(start_tiles < end_tiles);
    }
    struct Rectangle {
      u32 x_pixels;
      u32 y_pixels;
      u32 width_pixels;
      u32 height_pixels;
    };
    static constexpr u32 kMaxRectanglesWithoutCutout = 3;
    static constexpr u32 kMaxCutoutBorderRectangles = 4;
    static constexpr u32 kMaxRectanglesWithCutout =
        kMaxRectanglesWithoutCutout * kMaxCutoutBorderRectangles;
    /**
     * Returns the rectangles covering a tile range. A cutout can be specified for resolve clears,
     * to skip areas that are cleared to a single value anyway.
     *
     * @param start_tiles First tile of the range.
     * @param end_tiles Tile after the last one of the range.
     * @param base_tiles Base tile of the render target.
     * @param pitch_tiles Row pitch in tiles.
     * @param msaa_samples Multisample count of the render target.
     * @param is_64bpp True when the render target uses 64 bits per pixel.
     * @param rectangles_out Receives the rectangles.
     * @param cutout Area to exclude, may be null.
     * @return Number of rectangles written.
     */
    static u32 GetRangeRectangles(u32 start_tiles, u32 end_tiles, u32 base_tiles, u32 pitch_tiles,
                                  xenos::MSAASamples msaa_samples, bool is_64bpp,
                                  Rectangle* rectangles_out, const Rectangle* cutout = nullptr);
    u32 GetRectangles(u32 base_tiles, u32 pitch_tiles, xenos::MSAASamples msaa_samples,
                      bool is_64bpp, Rectangle* rectangles_out,
                      const Rectangle* cutout = nullptr) const {
      return GetRangeRectangles(start_tiles, end_tiles, base_tiles, pitch_tiles, msaa_samples,
                                is_64bpp, rectangles_out, cutout);
    }
    bool AreSourcesSame(const Transfer& other_transfer) const {
      return source == other_transfer.source &&
             host_depth_source == other_transfer.host_depth_source;
    }

   private:
    static u32 AddRectangle(const Rectangle& rectangle, Rectangle* rectangles_out,
                            const Rectangle* cutout = nullptr);
  };

  union HostDepthStoreRectangleConstant {
    u32 constant;
    struct {
      // Minus 1 because the maximum is 0x1FFF / 8, not 0x2000 / 8.
      u32 x_pixels_div_8 : xenos::kResolveSizeBits - 1 - xenos::kResolveAlignmentPixelsLog2;
      u32 y_pixels_div_8 : xenos::kResolveSizeBits - 1 - xenos::kResolveAlignmentPixelsLog2;
      u32 width_pixels_div_8_minus_1 : xenos::kResolveSizeBits - 1 -
                                       xenos::kResolveAlignmentPixelsLog2;
    };
    HostDepthStoreRectangleConstant() : constant(0) {
      REX_STATIC_ASSERT_SIZE(*this, sizeof(constant));
    }
  };

  union HostDepthStoreRenderTargetConstant {
    u32 constant;
    struct {
      u32 pitch_tiles : xenos::kEDRAMPitchTilesBits;
      u32 resolution_scale_x : 3;
      u32 resolution_scale_y : 3;
      /** Set when 2x MSAA is supported natively rather than through 4x. */
      u32 msaa_2x_supported : 1;
    };
    HostDepthStoreRenderTargetConstant() : constant(0) {
      REX_STATIC_ASSERT_SIZE(*this, sizeof(constant));
    }
  };

  struct HostDepthStoreConstants {
    HostDepthStoreRectangleConstant rectangle;
    HostDepthStoreRenderTargetConstant render_target;
  };

  /**
   * Row spans of a render target dump, in tiles. With rows == 1, row row_first spans
   * [row_first_start, row_last_end). With rows > 1, row row_first spans
   * [row_first_start, row_length_used), rows [row_first + 1, row_first + rows - 1) span
   * [row * pitch, row * pitch + row_length_used), and row row_first + rows - 1 spans
   * [row * pitch, row * pitch + row_last_end).
   */
  struct ResolveCopyDumpRectangle {
    RenderTarget* render_target;
    u32 row_first;
    u32 rows;
    u32 row_first_start;
    u32 row_last_end;
    ResolveCopyDumpRectangle(RenderTarget* render_target, u32 row_first, u32 rows,
                             u32 row_first_start, u32 row_last_end)
        : render_target(render_target),
          row_first(row_first),
          rows(rows),
          row_first_start(row_first_start),
          row_last_end(row_last_end) {}
    struct Dispatch {
      /** Base plus offset may exceed the EDRAM tile count when EDRAM addressing wraps. */
      u32 offset;
      u32 width_tiles;
      u32 height_tiles;
    };
    static constexpr u32 kMaxDispatches = 3;
    u32 GetDispatches(u32 pitch_tiles, u32 row_length_used, Dispatch* dispatches_out) const {
      if (!rows) {
        return 0;
      }
      u32 dispatch_count = 0;
      if (rows == 1 || row_first_start) {
        Dispatch& dispatch_first = dispatches_out[dispatch_count++];
        dispatch_first.offset = row_first * pitch_tiles + row_first_start;
        dispatch_first.width_tiles = (rows == 1 ? row_last_end : row_length_used) - row_first_start;
        dispatch_first.height_tiles = 1;
        if (rows == 1) {
          return dispatch_count;
        }
      }
      u32 mid_row_first = row_first + 1;
      u32 mid_rows = rows - 2;
      if (!row_first_start) {
        --mid_row_first;
        ++mid_rows;
      }
      if (row_last_end == row_length_used) {
        ++mid_rows;
      }
      if (mid_rows) {
        Dispatch& dispatch_mid = dispatches_out[dispatch_count++];
        dispatch_mid.offset = mid_row_first * pitch_tiles;
        dispatch_mid.width_tiles = row_length_used;
        dispatch_mid.height_tiles = mid_rows;
      }
      if (row_last_end != row_length_used) {
        Dispatch& dispatch_last = dispatches_out[dispatch_count++];
        dispatch_last.offset = (row_first + rows - 1) * pitch_tiles;
        dispatch_last.width_tiles = row_last_end;
        dispatch_last.height_tiles = 1;
      }
      return dispatch_count;
    }
  };

  /** Flattened dispatch extracted from a ResolveCopyDumpRectangle. */
  struct ResolveCopyDispatch {
    u32 rectangle_index;
    ResolveCopyDumpRectangle::Dispatch dispatch;
    ResolveCopyDispatch(u32 rectangle_index, const ResolveCopyDumpRectangle::Dispatch& dispatch)
        : rectangle_index(rectangle_index), dispatch(dispatch) {}
  };

  virtual u32 GetMaxRenderTargetWidth() const = 0;
  virtual u32 GetMaxRenderTargetHeight() const = 0;

  /**
   * Returns the height of a render target that is needed and can be created, within guest and host
   * limits. EDRAM base and 32bpp/64bpp are not considered, so one height serves all render targets
   * even if the implementation supports mixed size render targets. The implementation can then
   * disable individual render targets and let the others use the freed space without restarting the
   * render pass (on Vulkan the used height is set in VkFramebuffer).
   *
   * @param pitch_tiles_at_32bpp Pitch in tiles at 32bpp.
   * @param msaa_samples MSAA sample count.
   * @return Render target height.
   */
  u32 GetRenderTargetHeight(u32 pitch_tiles_at_32bpp, xenos::MSAASamples msaa_samples) const;

  virtual std::unique_ptr<RenderTarget> CreateRenderTarget(RenderTargetKey key) = 0;

  /**
   * Reports whether the depth buffer is encoded differently on the host, so naive aliasing may lose
   * precision. Host depth must be overwritten only if the new guest value differs from the current
   * host depth converted to the guest format, which mostly catches overwriting to clear the buffer.
   *
   * @param format Depth render target format.
   * @return True when the host encoding differs from the guest format.
   */
  virtual bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const = 0;

  void ResetAccumulatedRenderTargets() { are_accumulated_render_targets_valid_ = false; }
  RenderTarget* const* GetLastUpdateAccumulatedRenderTargets() const {
    REX_ASSERT(GetPath() == Path::kHostRenderTargets);
    return last_update_accumulated_render_targets_;
  }
  const std::vector<Transfer>* GetLastUpdateTransfers() const {
    REX_ASSERT(GetPath() == Path::kHostRenderTargets);
    return last_update_transfers_;
  }

  HostDepthStoreRenderTargetConstant GetHostDepthStoreRenderTargetConstant(
      u32 pitch_tiles, bool msaa_2x_supported) const {
    HostDepthStoreRenderTargetConstant constant;
    constant.pitch_tiles = pitch_tiles;
    // 3 bits for each.
    REX_ASSERT(GetDrawResolutionScaleX() <= 7);
    REX_ASSERT(GetDrawResolutionScaleY() <= 7);
    constant.resolution_scale_x = GetDrawResolutionScaleX();
    constant.resolution_scale_y = GetDrawResolutionScaleY();
    constant.msaa_2x_supported = static_cast<u32>(msaa_2x_supported);
    return constant;
  }
  void GetHostDepthStoreRectangleInfo(const Transfer::Rectangle& transfer_rectangle,
                                      xenos::MSAASamples msaa_samples,
                                      HostDepthStoreRectangleConstant& rectangle_constant_out,
                                      u32& group_count_x_out, u32& group_count_y_out) const;

  /**
   * Returns mappings between ranges within the tile rectangle (not the render target texture
   * rectangle, as textures may have any pitch) from ResolveInfo::GetCopyEDRAMTileSpan and the
   * render targets owning them.
   *
   * @param base First tile.
   * @param row_length Row length in tiles.
   * @param rows Row count.
   * @param pitch Row pitch in tiles.
   * @param rectangles_out Receives the mappings.
   */
  void GetResolveCopyRectanglesToDump(u32 base, u32 row_length, u32 rows, u32 pitch,
                                      std::vector<ResolveCopyDumpRectangle>& rectangles_out) const;
  void GetResolveCopyDispatchesToDump(u32 base, u32 row_length, u32 rows, u32 pitch,
                                      std::vector<ResolveCopyDumpRectangle>& rectangles_out,
                                      std::vector<ResolveCopyDispatch>& dispatches_out) const;

  /**
   * Sets up the render targets and transfers needed to clear in a resolve operation through a host
   * render target clear. resolve_info is expected to come from draw_state::GetResolveInfo.
   *
   * @param resolve_info Resolve information.
   * @param clear_rectangle_out Receives the area to clear.
   * @param depth_render_target_out Receives the depth render target to clear.
   * @param depth_transfers_out Receives the transfers needed for the depth render target.
   * @param color_render_target_out Receives the color render target to clear.
   * @param color_transfers_out Receives the transfers needed for the color render target.
   * @return True when any clears need to be done (false when there are none and on error).
   */
  bool PrepareHostRenderTargetsResolveClear(const draw_state::ResolveInfo& resolve_info,
                                            Transfer::Rectangle& clear_rectangle_out,
                                            RenderTarget*& depth_render_target_out,
                                            std::vector<Transfer>& depth_transfers_out,
                                            RenderTarget*& color_render_target_out,
                                            std::vector<Transfer>& color_transfers_out);

  virtual void RequestPixelShaderInterlockBarrier() {}

  /** Called by the implementation when interlocked writes to all of the EDRAM are committed with a
   * memory barrier. */
  void PixelShaderInterlockFullEDRAMBarrierPlaced();

 private:
  const xenos::RegisterFile& register_file_;
  u32 draw_resolution_scale_x_;
  u32 draw_resolution_scale_y_;

  DrawExtentEstimator draw_extent_estimator_;

  struct OwnershipRange {
    u32 end_tiles;
    /**
     * Last using render target. Stored as a key rather than a pointer since pixel shader interlock
     * has no host render targets and uses ownership to place barriers.
     */
    RenderTargetKey render_target;
    /**
     * Last host depth render targets that used the range even if another render target used it
     * since, tracked only when the format has a different encoding on the host. They are tracked
     * separately and overwritten when the host value converted to the guest format becomes out of
     * sync with the guest value. unorm24 and float24 render targets are tracked separately even
     * when the host emulates both with float32 (Vulkan on AMD), so switching between them for the
     * same depth data (most clears go through unorm24 without a viewport) is not destructive.
     * f32tof24(host_f32) == guest_f24 does not imply f32tou24(host_f32) == guest_u24, so aliasing
     * float24 with unorm24 through the same float32 buffer would drop the precision to that of
     * unorm24 with a wrong value. Empty when the range is unused (render_target.IsEmpty() == true).
     */
    RenderTargetKey host_depth_render_target_unorm24;
    RenderTargetKey host_depth_render_target_float24;
    OwnershipRange(u32 end_tiles, RenderTargetKey render_target,
                   RenderTargetKey host_depth_render_target_unorm24,
                   RenderTargetKey host_depth_render_target_float24)
        : end_tiles(end_tiles),
          render_target(render_target),
          host_depth_render_target_unorm24(host_depth_render_target_unorm24),
          host_depth_render_target_float24(host_depth_render_target_float24) {}
    const RenderTargetKey& GetHostDepthRenderTarget(
        xenos::DepthRenderTargetFormat resource_format) const {
      REX_ASSERT(resource_format == xenos::DepthRenderTargetFormat::kD24S8 ||
                     resource_format == xenos::DepthRenderTargetFormat::kD24FS8,
                 "Illegal resource format");
      return resource_format == xenos::DepthRenderTargetFormat::kD24S8
                 ? host_depth_render_target_unorm24
                 : host_depth_render_target_float24;
    }
    RenderTargetKey& GetHostDepthRenderTarget(xenos::DepthRenderTargetFormat resource_format) {
      return const_cast<RenderTargetKey&>(
          const_cast<const OwnershipRange*>(this)->GetHostDepthRenderTarget(resource_format));
    }
    bool IsOwnedBy(RenderTargetKey key, bool host_depth_encoding_different) const {
      if (render_target != key) {
        return false;
      }
      if (host_depth_encoding_different && !key.is_depth &&
          GetHostDepthRenderTarget(key.GetDepthFormat()) != key) {
        return false;
      }
      return true;
    }
    bool AreOwnersSame(const OwnershipRange& other_range) const {
      return render_target == other_range.render_target &&
             host_depth_render_target_unorm24 == other_range.host_depth_render_target_unorm24 &&
             host_depth_render_target_float24 == other_range.host_depth_render_target_float24;
    }
  };

  xenos::ColorRenderTargetFormat GetColorResourceFormat(
      xenos::ColorRenderTargetFormat format) const {
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        !IsGammaFormatHostStorageSeparate()) {
      return xenos::ColorRenderTargetFormat::k_8_8_8_8;
    }
    return xenos::GetStorageColorFormat(format);
  }

  RenderTarget* GetOrCreateRenderTarget(RenderTargetKey key);

  /**
   * Reports whether changing ownership of the range to the render target would require transferring
   * data. Used primarily for barrier placement on the pixel shader interlock path, where transfers
   * do not copy but barriers are needed before accessing ranges written before the barrier and
   * addressed by different render target independent rasterization pixel positions.
   *
   * @param dest Render target taking ownership.
   * @param start_tiles_base_relative First tile, relative to the base.
   * @param length_tiles Length in tiles.
   * @return True when transfers would be required.
   */
  bool WouldOwnershipChangeRequireTransfers(RenderTargetKey dest, u32 start_tiles_base_relative,
                                            u32 length_tiles) const;
  /**
   * Updates ownership_ranges_ and adds the transfers needed for the change to transfers_append_out
   * when it is not null.
   *
   * @param dest Render target taking ownership.
   * @param start_tiles_base_relative First tile, relative to the base.
   * @param length_tiles Length in tiles.
   * @param transfers_append_out Receives the transfers, may be null.
   * @param resolve_clear_cutout Area excluded from transfers, may be null.
   */
  void ChangeOwnership(RenderTargetKey dest, u32 start_tiles_base_relative, u32 length_tiles,
                       std::vector<Transfer>* transfers_append_out,
                       const Transfer::Rectangle* resolve_clear_cutout = nullptr);

  std::unordered_map<RenderTargetKey, std::unique_ptr<RenderTarget>, RenderTargetKey::Hasher>
      /** May hold nullptr after a failed creation, to prevent attempting to create it twice. */
      render_targets_;

  std::map<u32, OwnershipRange>
      /**
       * Host render targets holding the most up to date tile data. Has no gaps, unused parts are
       * represented by empty render target keys.
       */
      ownership_ranges_;

  RenderTarget*
      last_update_used_render_targets_[1 +
                                       /**
                                        * Render targets of the last successful update draw. Index 0
                                        * is depth, colors start from 1, nullptr when not bound.
                                        * Valid only on non pixel shader interlock paths.
                                        */
                                       xenos::kMaxColorRenderTargets];
  RenderTarget* last_update_accumulated_render_targets_
      /**
       * Render targets of the last successful update or earlier ones, unless a different or new one
       * was bound or surface info changed, to avoid needless switching (undesirable on tile based
       * GPUs) when depth and stencil test or color writes are toggled. Must also decide whether
       * depth and stencil or a color render target may be enabled in the pipeline for the draw.
       * Valid only on non pixel shader interlock paths.
       */
      [1 + xenos::kMaxColorRenderTargets];
  bool are_accumulated_render_targets_valid_ =
      /**
       * When false, the next update copies last_update_used_render_targets_ to
       * last_update_accumulated_render_targets_, as keeping the previously bound render targets is
       * not beneficial or is incorrect.
       */
      false;
  std::vector<Transfer>
      last_update_transfers_[1 +
                             /**
                              * After an update, even an unsuccessful one, the ownership transfer
                              * sources needed for each current render target, ordered so all
                              * transfers of one source are consecutive.
                              */
                             xenos::kMaxColorRenderTargets];
};

}  // namespace rex::gpu
