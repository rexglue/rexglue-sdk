/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_EDRAM_MSL_H_
#define XENIA_GPU_METAL_METAL_EDRAM_MSL_H_

namespace rex {
namespace graphics {
namespace metal {

// The canonical color/depth/stencil EDRAM materialize/pack MSL, extracted as a
// SHARED production source. It is compiled verbatim by BOTH the conformance harness
// (metal_kernel_conformance_test.mm, which appends its extra kernels after it)
// and the live render-target cache's cross-MSAA path, so the tested helpers and
// the live path are byte-identical and cannot drift -- the ladder's core
// guarantee. Every function here is proven by the [l1][metal][...][depth] cases:
// float24 over the full [0,2) via *0.5//*2, unorm24 exact incl. near-1.0, the
// complete (d24<<8)|stencil word via bit-plane stencil, per-sample 4x, and the
// mixed 4x->1x->4x cross-MSAA chain.
//
// Self-contained (has the metal_stdlib prelude, the shared addresser, and
// SurfaceParams) so it compiles as a standalone MTLLibrary for the live path.
//
// SurfaceParams.is_depth encodes BOTH depth-ness and the guest format for the
// depth kernels: 2 = float24 (kD24FS8, [0,2) via *0.5//*2), 1 = unorm24
// (kD24S8, [0,1]); any nonzero value triggers the 40-column depth swap in the
// addresser. Color kernels pass is_depth = 0.
static constexpr const char* kEdramMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

// Host samples per 32bpp EDRAM tile: 80x16 times the draw resolution scale
// (implement-metal-draw-resolution-scaling). The live cache defines these per
// session (MetalRenderTargetCache::EdramMslCompileOptions); a compile without
// them, such as the conformance harness, gets the unscaled 1x tile.
#ifndef XE_EDRAM_TILE_WIDTH_SAMPLES
#define XE_EDRAM_TILE_WIDTH_SAMPLES 80u
#endif
#ifndef XE_EDRAM_TILE_HEIGHT_SAMPLES
#define XE_EDRAM_TILE_HEIGHT_SAMPLES 16u
#endif

inline uint2 xe_edram_msaa_shifts(uint msaa) {
  return uint2(msaa >= 2u ? 1u : 0u, msaa >= 1u ? 1u : 0u);
}
// Metal's native sample indices use the D3D10-standard position order, while
// Xenos EDRAM sample slots follow the sub-pixel storage order consumed by
// xe_edram_offset_bytes. Convert only at the real multisample attachment
// boundary: native 2x bottom/top -> guest top/bottom, and native 4x
// TL/TR/BL/BR -> guest TL/BL/TR/BR. Canonical and array-backed callers already
// carry guest sample indices and must not use this conversion.
inline uint xe_metal_sample_to_guest(uint msaa, uint sample) {
  if (msaa == 1u) {
    return sample ^ 1u;
  }
  if (msaa == 2u) {
    return sample == 1u ? 2u : (sample == 2u ? 1u : sample);
  }
  return sample;
}
inline uint xe_edram_depth_swap_x(uint x, uint tile_width) {
  uint half_width = tile_width >> 1u;
  return x >= half_width ? x - half_width : x + half_width;
}

// Shared addresser: edram.xesli XeEdramOffsetBytes with wrap=false, at the
// session's draw resolution scale (the tile is 80x16 samples at 32bpp times the
// scale; px/py are host pixels). One MSL definition used by BOTH the
// offset-parity kernel and the canonical pack/unpack kernels, so they cannot
// drift. msaa: 0=1x,1=2x,2=4x.
inline uint xe_edram_offset_bytes(uint px, uint py, uint base, uint pitch,
                                  uint msaa, uint is_depth, uint fmt_log2,
                                  uint sample) {
  uint2 s = uint2(px, py) << xe_edram_msaa_shifts(msaa);
  s += (uint2(sample, sample) >> uint2(1u, 0u)) & 1u;
  uint2 tsize32 =
      uint2(XE_EDRAM_TILE_WIDTH_SAMPLES, XE_EDRAM_TILE_HEIGHT_SAMPLES);
  uint2 tsize = tsize32 >> uint2(fmt_log2, 0u);
  uint2 toff = s / tsize;
  uint b = base + toff.y * pitch + toff.x;
  s -= toff * tsize;
  if (is_depth != 0u) {
    s.x = xe_edram_depth_swap_x(s.x, tsize.x);
  }
  uint address = b * (tsize32.x * tsize32.y) +
                 ((s.y * tsize.x + s.x) << fmt_log2);
  return address << 2;
}

// The word of guest sample `guest_sample` of the pixel whose sample 0 is at
// word idx0. A pixel's samples are adjacent sample columns / rows of one tile
// (tiles are an even number of samples wide and tall, and the depth half swap
// keeps an aligned sample pair together); a tile row is
// XE_EDRAM_TILE_WIDTH_SAMPLES words at either bpp. The per-pixel pack kernels
// compute the range test and the address once per pixel with this.
inline uint xe_pixel_sample_word(uint idx0, uint guest_sample, uint fmt_log2) {
  return idx0 + (((guest_sample >> 1u) & 1u) << fmt_log2) +
         (guest_sample & 1u) * XE_EDRAM_TILE_WIDTH_SAMPLES;
}

// Per-surface constants for pack/unpack (one surface per dispatch). `words` is
// the canonical buffer size in 32-bit words, so a bounds guard can skip an
// over-dispatch instead of reading/writing out of range.
struct SurfaceParams { uint base, pitch, msaa, is_depth, fmt_log2, words; };

inline float4 xe_unpack_8888(uint word) {
  return float4(word & 0xFFu, (word >> 8u) & 0xFFu,
                (word >> 16u) & 0xFFu, (word >> 24u) & 0xFFu) *
         (1.0 / 255.0);
}
inline uint xe_pack_8888(float4 color) {
  uint4 bytes = uint4(round(color * 255.0)) & 0xFFu;
  return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}

// Color storage classes used by the pinned Katamari + Red Dead corpus. Values
// are the Xenos ColorRenderTargetFormat enum after GetStorageColorFormat:
// 0 = 8_8_8_8 (gamma has the same raw storage), 3 = 2_10_10_10_FLOAT,
// 4 = 16_16 fixed, 14 = 32_FLOAT.
inline float xe_7e3_to_f32(uint value) {
  value &= 0x3FFu;
  if (value == 0u) return 0.0;
  uint mantissa = value & 0x7Fu;
  uint exponent = value >> 7u;
  if (exponent == 0u) {
    uint lz = clz(mantissa) - 24u;
    exponent = 1u - lz;
    mantissa = (mantissa << lz) & 0x7Fu;
  }
  return as_type<float>(((exponent + 124u) << 23u) | (mantissa << 16u));
}
inline uint xe_f32_to_7e3(float value) {
  if (!(value > 0.0)) return 0u;
  if (value >= 31.875) return 0x3FFu;
  uint bits = as_type<uint>(value);
  int exponent = int((bits >> 23u) & 0xFFu) - 124;
  uint mantissa = (bits >> 16u) & 0x7Fu;
  uint round_up = (bits >> 15u) & 1u;
  if (exponent <= 0) {
    uint shift = uint(1 - exponent);
    if (shift > 8u) return 0u;
    uint full = 0x80u | mantissa;
    uint result = (full >> shift) + ((full >> (shift - 1u)) & 1u);
    return min(result, 0x7Fu);
  }
  mantissa += round_up;
  if (mantissa > 0x7Fu) {
    mantissa = 0u;
    ++exponent;
    if (exponent >= 8) return 0x3FFu;
  }
  return (uint(exponent) << 7u) | mantissa;
}
// Xenos float16 (16_16_FLOAT / 16_16_16_16_FLOAT render targets, 16-bit float
// texture formats on the resolve path) has no Inf or NaN: exponent 31 holds
// finite values up to +-131008 (Canary 3ff230d23; CPU oracle
// xenos::Float16ExtendedRangeTo32 / Float32ToFloat16ExtendedRange, pinned by
// the L0/L1 conformance ladder). The host attachments keep the raw bits
// through their Uint sync views; these conversions serve resolves, clears and
// the float working views, so an exponent-31 code survives a resolve instead
// of collapsing to 0x7C00.
inline float xe_f16_ext_to_f32(uint h) {
  h &= 0xFFFFu;
  if ((h & 0x7C00u) == 0x7C00u) {
    return 2.0 * float(as_type<half>(ushort(h - 0x0400u)));
  }
  return float(as_type<half>(ushort(h)));
}
inline uint xe_f32_to_f16_ext(float v) {
  // Integer NaN test: fast-math may fold isnan().
  if ((as_type<uint>(v) & 0x7FFFFFFFu) > 0x7F800000u) {
    return 0u;
  }
  uint h = uint(as_type<ushort>(half(v)));
  if ((h & 0x7C00u) == 0x7C00u) {
    float c = clamp(v, -131008.0, 131008.0) * 0.5;
    h = uint(as_type<ushort>(half(c))) + 0x0400u;
  }
  return h;
}
inline float4 xe_unpack_color32(uint word, uint format) {
  switch (format) {
    default:
    case 0u:
      return xe_unpack_8888(word);
    case 2u:
      return float4(float(word & 0x3FFu) / 1023.0,
                    float((word >> 10u) & 0x3FFu) / 1023.0,
                    float((word >> 20u) & 0x3FFu) / 1023.0,
                    float(word >> 30u) / 3.0);
    case 3u:
      return float4(xe_7e3_to_f32(word), xe_7e3_to_f32(word >> 10u),
                    xe_7e3_to_f32(word >> 20u), float(word >> 30u) / 3.0);
    case 4u: {
      int2 fixed = int2(as_type<int>(word << 16u) >> 16,
                        as_type<int>(word) >> 16);
      return float4(float2(fixed) * (1.0 / 1024.0), 0.0, 1.0);
    }
    case 6u:
      return float4(xe_f16_ext_to_f32(word), xe_f16_ext_to_f32(word >> 16u),
                    0.0, 0.0);
    case 14u:
      return float4(as_type<float>(word), 0.0, 0.0, 0.0);
  }
}
inline int xe_f32_to_fixed16(float value) {
  // Signed 16-bit EDRAM components have 10 fractional bits (Canary 42c2b880e,
  // matched to real hardware): the value scaled by 1024 rounds half up, also
  // for negatives, clamped to [-32, 32767/1024]; -32 packs to 0x8000.
  value = clamp(value, -32.0, 32767.0 / 1024.0);
  return int(floor(value * 1024.0 + 0.5));
}
inline uint xe_pack_color32(float4 color, uint format) {
  switch (format) {
    default:
    case 0u:
      return xe_pack_8888(color);
    case 2u:
      return uint(round(saturate(color.r) * 1023.0)) |
             (uint(round(saturate(color.g) * 1023.0)) << 10u) |
             (uint(round(saturate(color.b) * 1023.0)) << 20u) |
             (uint(round(saturate(color.a) * 3.0)) << 30u);
    case 3u:
      return xe_f32_to_7e3(color.r) | (xe_f32_to_7e3(color.g) << 10u) |
             (xe_f32_to_7e3(color.b) << 20u) |
             (uint(round(saturate(color.a) * 3.0)) << 30u);
    case 4u: {
      int2 fixed = int2(xe_f32_to_fixed16(color.r),
                        xe_f32_to_fixed16(color.g));
      return (uint(fixed.x) & 0xFFFFu) | (uint(fixed.y) << 16u);
    }
    case 6u:
      return xe_f32_to_f16_ext(color.r) | (xe_f32_to_f16_ext(color.g) << 16u);
    case 14u:
      return as_type<uint>(color.r);
  }
}
// k_8_8_8_8_GAMMA (format 1) working-view helpers. GAMMA render targets store
// piecewise-linear (PWL) gamma-encoded bytes in EDRAM; xe_unpack_color32 /
// xe_pack_color32 stay raw-byte for canonical resolves, so the gamma
// decode/encode used by the generic color working-view kernels lives here,
// dispatched only for format == 1u.
inline float xe_gamma_byte_to_linear(float b) {
  if (b < 64.0) return (b + 0.5) / 1023.0;
  if (b < 96.0) return (b - 31.5) / 511.5;
  if (b < 192.0) return (b - 63.5) / 255.75;
  return (b - 127.5) / 127.875;
}
inline float4 xe_unpack_color32_working_gamma(uint word) {
  float4 bytes = float4(word & 0xFFu, (word >> 8u) & 0xFFu,
                        (word >> 16u) & 0xFFu, (word >> 24u) & 0xFFu);
  return float4(xe_gamma_byte_to_linear(bytes.x),
               xe_gamma_byte_to_linear(bytes.y),
               xe_gamma_byte_to_linear(bytes.z), bytes.w / 255.0);
}
inline float xe_linear_to_gamma_byte(float linear) {
  float scale, offset;
  if (linear < 64.0 / 1023.0) {
    scale = 1023.0;
    offset = 0.0;
  } else if (linear < 128.0 / 1023.0) {
    scale = 1023.0 / 2.0;
    offset = 32.0 / 255.0;
  } else if (linear < 512.0 / 1023.0) {
    scale = 1023.0 / 4.0;
    offset = 64.0 / 255.0;
  } else {
    scale = 1023.0 / 8.0;
    offset = 128.0 / 255.0;
  }
  return trunc(linear * scale) / 255.0 + offset;
}
inline uint xe_pack_color32_working_gamma(float4 color) {
  float3 c = saturate(color.rgb);
  float4 encoded = float4(xe_linear_to_gamma_byte(c.r),
                          xe_linear_to_gamma_byte(c.g),
                          xe_linear_to_gamma_byte(c.b), saturate(color.a));
  return xe_pack_8888(encoded);
}
// Dispatch to the gamma working-view helpers for format 1, raw numeric
// helpers for everything else.
inline float4 xe_unpack_color32_working(uint word, uint format) {
  return format == 1u ? xe_unpack_color32_working_gamma(word)
                      : xe_unpack_color32(word, format);
}
inline uint xe_pack_color32_working(float4 color, uint format) {
  return format == 1u ? xe_pack_color32_working_gamma(color)
                      : xe_pack_color32(color, format);
}
inline float4 xe_unpack_color64(uint2 words, uint format) {
  switch (format) {
    default:
    case 5u: {
      int4 fixed = int4(as_type<int>(words.x << 16u) >> 16,
                        as_type<int>(words.x) >> 16,
                        as_type<int>(words.y << 16u) >> 16,
                        as_type<int>(words.y) >> 16);
      return float4(fixed) * (1.0 / 1024.0);
    }
    case 7u:
      return float4(xe_f16_ext_to_f32(words.x), xe_f16_ext_to_f32(words.x >> 16u),
                    xe_f16_ext_to_f32(words.y),
                    xe_f16_ext_to_f32(words.y >> 16u));
    case 15u:
      return float4(as_type<float>(words.x), as_type<float>(words.y), 0.0,
                    0.0);
  }
}
inline uint2 xe_pack_color64(float4 color, uint format) {
  switch (format) {
    default:
    case 5u: {
      int4 fixed = int4(xe_f32_to_fixed16(color.r),
                        xe_f32_to_fixed16(color.g),
                        xe_f32_to_fixed16(color.b),
                        xe_f32_to_fixed16(color.a));
      return uint2((uint(fixed.x) & 0xFFFFu) |
                       (uint(fixed.y) << 16u),
                   (uint(fixed.z) & 0xFFFFu) |
                       (uint(fixed.w) << 16u));
    }
    case 7u:
      return uint2(
          xe_f32_to_f16_ext(color.r) | (xe_f32_to_f16_ext(color.g) << 16u),
          xe_f32_to_f16_ext(color.b) | (xe_f32_to_f16_ext(color.a) << 16u));
    case 15u:
      return uint2(as_type<uint>(color.r), as_type<uint>(color.g));
  }
}
inline uint4 xe_unpack_color32_raw(uint word, uint format) {
  if (format == 6u) {
    return uint4(word & 0xFFFFu, word >> 16u, 0u, 0u);
  }
  return uint4(word, 0u, 0u, 0u);
}
inline uint xe_pack_color32_raw(uint4 color, uint format) {
  if (format == 6u) {
    return (color.x & 0xFFFFu) | ((color.y & 0xFFFFu) << 16u);
  }
  return color.x;
}
inline uint4 xe_unpack_color64_raw(uint2 words, uint format) {
  if (format == 7u) {
    return uint4(words.x & 0xFFFFu, words.x >> 16u,
                 words.y & 0xFFFFu, words.y >> 16u);
  }
  return uint4(words.x, words.y, 0u, 0u);
}
inline uint2 xe_pack_color64_raw(uint4 color, uint format) {
  if (format == 7u) {
    return uint2((color.x & 0xFFFFu) | ((color.y & 0xFFFFu) << 16u),
                 (color.z & 0xFFFFu) | ((color.w & 0xFFFFu) << 16u));
  }
  return color.xy;
}
inline uint xe_pack_depth_stencil(uint depth24, uint stencil) {
  return ((depth24 & 0xFFFFFFu) << 8u) | (stencil & 0xFFu);
}
inline uint xe_depth24(uint word) { return word >> 8u; }
inline uint xe_stencil(uint word) { return word & 0xFFu; }
inline uint xe_stencil_bit_mask(uint bit_index) {
  return 1u << (bit_index & 7u);
}

struct AuthorityRangeParams {
  uint start_tiles;
  uint end_tiles;
};
inline uint xe_authority_tile_of(uint px, uint py, constant SurfaceParams& sp,
                                 uint sample) {
  uint2 sample_xy = uint2(px, py) << xe_edram_msaa_shifts(sp.msaa);
  sample_xy += (uint2(sample, sample) >> uint2(1u, 0u)) & 1u;
  uint tile_width_samples = XE_EDRAM_TILE_WIDTH_SAMPLES >> sp.fmt_log2;
  return sp.base + (sample_xy.y / XE_EDRAM_TILE_HEIGHT_SAMPLES) * sp.pitch +
         sample_xy.x / tile_width_samples;
}
inline bool xe_authority_in_range(uint px, uint py,
                                  constant SurfaceParams& sp, uint sample,
                                  constant AuthorityRangeParams& range) {
  uint tile = xe_authority_tile_of(px, py, sp, sample);
  return tile >= range.start_tiles && tile < range.end_tiles;
}

// Canonical RGBA8Unorm materialize/pack kernels. These are shared verbatim by
// the live cache and the Metal conformance suite.
kernel void materialize_8888(
    device const uint* canonical [[buffer(0)]],
    texture2d<float, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  uint word = idx < sp.words ? canonical[idx] : 0u;
  tex.write(xe_unpack_8888(word), gid);
}
kernel void pack_8888(
    texture2d<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  if (idx < sp.words) canonical[idx] = xe_pack_8888(tex.read(gid));
}

// Generic single-sample color working-view conversion for the formats used by
// the deterministic corpus. The attachment format supplies the lossless host
// container (RGBA8Unorm, RGBA16Float, RG32Float, or R32Float); `format` supplies
// the guest raw-word interpretation.
kernel void materialize_color(
    device const uint* canonical [[buffer(0)]],
    texture2d<float, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  tex.write(
      xe_unpack_color32_working(idx < sp.words ? canonical[idx] : 0u, format),
      gid);
}
kernel void pack_color(
    texture2d<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_working(tex.read(gid), format);
  }
}
kernel void materialize_color_range(
    device const uint* canonical [[buffer(0)]],
    texture2d<float, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  tex.write(
      xe_unpack_color32_working(idx < sp.words ? canonical[idx] : 0u, format),
      gid);
}
kernel void pack_color_range(
    texture2d<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_working(tex.read(gid), format);
  }
}
kernel void materialize_color64_range(
    device const uint* canonical [[buffer(0)]],
    texture2d<float, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, 0u) >> 2u;
  uint2 words = idx + 1u < sp.words
                    ? uint2(canonical[idx], canonical[idx + 1u])
                    : uint2(0u);
  tex.write(xe_unpack_color64(words, format), gid);
}
kernel void pack_color64_range(
    texture2d<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, 0u) >> 2u;
  if (idx + 1u < sp.words) {
    uint2 words = xe_pack_color64(tex.read(gid), format);
    canonical[idx] = words.x;
    canonical[idx + 1u] = words.y;
  }
}
kernel void materialize_color_raw_range(
    device const uint* canonical [[buffer(0)]],
    texture2d<uint, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u) >> 2u;
  uint word = idx < sp.words ? canonical[idx] : 0u;
  tex.write(xe_unpack_color32_raw(word, format), gid);
}
kernel void pack_color_raw_range(
    texture2d<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, 0u) >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_raw(tex.read(gid), format);
  }
}
kernel void materialize_color64_raw_range(
    device const uint* canonical [[buffer(0)]],
    texture2d<uint, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, 0u) >> 2u;
  uint2 words = idx + 1u < sp.words
                    ? uint2(canonical[idx], canonical[idx + 1u])
                    : uint2(0u);
  tex.write(xe_unpack_color64_raw(words, format), gid);
}
kernel void pack_color64_raw_range(
    texture2d<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint2 gid = uint2(xe_tid.x, xe_tid.y + row_start);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      !xe_authority_in_range(gid.x, gid.y, sp, 0u, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, 0u) >> 2u;
  if (idx + 1u < sp.words) {
    uint2 words = xe_pack_color64_raw(tex.read(gid), format);
    canonical[idx] = words.x;
    canonical[idx + 1u] = words.y;
  }
}

// Array-slice per-sample representation used by the isolated compute oracle.
kernel void materialize_8888_msaa(
    device const uint* canonical [[buffer(0)]],
    texture2d_array<float, access::write> tex [[texture(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_array_size()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, gid.z);
  uint idx = off >> 2u;
  uint word = idx < sp.words ? canonical[idx] : 0u;
  tex.write(xe_unpack_8888(word), uint2(gid.x, gid.y), gid.z);
}
kernel void pack_8888_msaa(
    texture2d_array<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_array_size()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, gid.z);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_8888(tex.read(uint2(gid.x, gid.y), gid.z));
  }
}

// Real multisample attachment representation used by the live path: sample-
// rate materialize and explicit per-sample compute packing.
struct ColorVOut { float4 pos [[position]]; };
vertex ColorVOut xe_color_vs(uint vid [[vertex_id]]) {
  ColorVOut out;
  float2 t = float2(float((vid << 1) & 2), float(vid & 2));
  out.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
  return out;
}
fragment float4 materialize_ms_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  uint off = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 0u, guest_sample);
  uint idx = off >> 2u;
  return xe_unpack_8888(idx < sp.words ? canonical[idx] : 0u);
}
kernel void pack_ms(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples()) return;
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, guest_sample);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_8888(tex.read(uint2(gid.x, gid.y), gid.z));
  }
}

// Format-generic real-MSAA color working-view conversions. The fragment runs
// once per covered sample because it consumes [[sample_id]], preserving every
// Xenos physical sample in the actual multisample attachment. The compute pack
// explicitly reads each sample back into the canonical physical layout.
fragment float4 materialize_color_ms_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  uint off = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 0u, guest_sample);
  uint idx = off >> 2u;
  return xe_unpack_color32_working(idx < sp.words ? canonical[idx] : 0u,
                                   format);
}
kernel void pack_color_ms(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples()) return;
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, guest_sample);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_working(
        tex.read(uint2(gid.x, gid.y), gid.z), format);
  }
}
fragment float4 materialize_color_ms_range_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  if (!xe_authority_in_range(pixel.x, pixel.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint off = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 0u, guest_sample);
  uint idx = off >> 2u;
  return xe_unpack_color32_working(idx < sp.words ? canonical[idx] : 0u,
                                   format);
}
kernel void pack_color_ms_range(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint3 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(xe_tid.x, xe_tid.y + row_start, xe_tid.z);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, guest_sample);
  uint idx = off >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_working(
        tex.read(uint2(gid.x, gid.y), gid.z), format);
  }
}
// pack_color_ms_range with one thread per pixel (see xe_pixel_sample_word).
kernel void pack_color_ms_range_px(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 tid [[thread_position_in_grid]]) {
  uint2 px = uint2(tid.x, tid.y + row_start);
  if (px.x >= tex.get_width() || px.y >= tex.get_height() ||
      !xe_authority_in_range(px.x, px.y, sp, 0u, range)) return;
  uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch, sp.msaa,
                                    0u, 0u, 0u) >> 2u;
  for (uint ms = 0u; ms < tex.get_num_samples(); ++ms) {
    uint idx = xe_pixel_sample_word(
        idx0, xe_metal_sample_to_guest(sp.msaa, ms), 0u);
    if (idx < sp.words) {
      canonical[idx] = xe_pack_color32_working(tex.read(px, ms), format);
    }
  }
}
fragment float4 materialize_color64_ms_range_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  if (!xe_authority_in_range(pixel.x, pixel.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint idx = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 1u, guest_sample) >> 2u;
  uint2 words = idx + 1u < sp.words
                    ? uint2(canonical[idx], canonical[idx + 1u])
                    : uint2(0u);
  return xe_unpack_color64(words, format);
}
kernel void pack_color64_ms_range(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint3 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(xe_tid.x, xe_tid.y + row_start, xe_tid.z);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, guest_sample) >> 2u;
  if (idx + 1u < sp.words) {
    uint2 words = xe_pack_color64(
        tex.read(uint2(gid.x, gid.y), gid.z), format);
    canonical[idx] = words.x;
    canonical[idx + 1u] = words.y;
  }
}
// pack_color64_ms_range with one thread per pixel (see xe_pixel_sample_word).
kernel void pack_color64_ms_range_px(
    texture2d_ms<float, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 tid [[thread_position_in_grid]]) {
  uint2 px = uint2(tid.x, tid.y + row_start);
  if (px.x >= tex.get_width() || px.y >= tex.get_height() ||
      !xe_authority_in_range(px.x, px.y, sp, 0u, range)) return;
  uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch, sp.msaa,
                                    0u, 1u, 0u) >> 2u;
  for (uint ms = 0u; ms < tex.get_num_samples(); ++ms) {
    uint idx = xe_pixel_sample_word(
        idx0, xe_metal_sample_to_guest(sp.msaa, ms), 1u);
    if (idx + 1u < sp.words) {
      uint2 words = xe_pack_color64(tex.read(px, ms), format);
      canonical[idx] = words.x;
      canonical[idx + 1u] = words.y;
    }
  }
}
fragment uint4 materialize_color_raw_ms_range_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  if (!xe_authority_in_range(pixel.x, pixel.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint idx = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 0u, guest_sample) >> 2u;
  uint word = idx < sp.words ? canonical[idx] : 0u;
  return xe_unpack_color32_raw(word, format);
}
kernel void pack_color_raw_ms_range(
    texture2d_ms<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint3 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(xe_tid.x, xe_tid.y + row_start, xe_tid.z);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 0u, guest_sample) >> 2u;
  if (idx < sp.words) {
    canonical[idx] = xe_pack_color32_raw(
        tex.read(uint2(gid.x, gid.y), gid.z), format);
  }
}
// pack_color_raw_ms_range with one thread per pixel (see xe_pixel_sample_word).
kernel void pack_color_raw_ms_range_px(
    texture2d_ms<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 tid [[thread_position_in_grid]]) {
  uint2 px = uint2(tid.x, tid.y + row_start);
  if (px.x >= tex.get_width() || px.y >= tex.get_height() ||
      !xe_authority_in_range(px.x, px.y, sp, 0u, range)) return;
  uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch, sp.msaa,
                                    0u, 0u, 0u) >> 2u;
  for (uint ms = 0u; ms < tex.get_num_samples(); ++ms) {
    uint idx = xe_pixel_sample_word(
        idx0, xe_metal_sample_to_guest(sp.msaa, ms), 0u);
    if (idx < sp.words) {
      canonical[idx] = xe_pack_color32_raw(tex.read(px, ms), format);
    }
  }
}
fragment uint4 materialize_color64_raw_ms_range_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    uint sample [[sample_id]]) {
  uint2 pixel = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sample);
  if (!xe_authority_in_range(pixel.x, pixel.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint idx = xe_edram_offset_bytes(pixel.x, pixel.y, sp.base, sp.pitch,
                                   sp.msaa, 0u, 1u, guest_sample) >> 2u;
  uint2 words = idx + 1u < sp.words
                    ? uint2(canonical[idx], canonical[idx + 1u])
                    : uint2(0u);
  return xe_unpack_color64_raw(words, format);
}
kernel void pack_color64_raw_ms_range(
    texture2d_ms<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint3 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(xe_tid.x, xe_tid.y + row_start, xe_tid.z);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint idx = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   0u, 1u, guest_sample) >> 2u;
  if (idx + 1u < sp.words) {
    uint2 words = xe_pack_color64_raw(
        tex.read(uint2(gid.x, gid.y), gid.z), format);
    canonical[idx] = words.x;
    canonical[idx + 1u] = words.y;
  }
}
// pack_color64_raw_ms_range with one thread per pixel (see xe_pixel_sample_word).
kernel void pack_color64_raw_ms_range_px(
    texture2d_ms<uint, access::read> tex [[texture(0)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant uint& format [[buffer(2)]],
    constant AuthorityRangeParams& range [[buffer(3)]],
    constant uint& row_start [[buffer(7)]],
    uint2 tid [[thread_position_in_grid]]) {
  uint2 px = uint2(tid.x, tid.y + row_start);
  if (px.x >= tex.get_width() || px.y >= tex.get_height() ||
      !xe_authority_in_range(px.x, px.y, sp, 0u, range)) return;
  uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch, sp.msaa,
                                    0u, 1u, 0u) >> 2u;
  for (uint ms = 0u; ms < tex.get_num_samples(); ++ms) {
    uint idx = xe_pixel_sample_word(
        idx0, xe_metal_sample_to_guest(sp.msaa, ms), 1u);
    if (idx + 1u < sp.words) {
      uint2 words = xe_pack_color64_raw(tex.read(px, ms), format);
      canonical[idx] = words.x;
      canonical[idx + 1u] = words.y;
    }
  }
}

inline float xe_f20e4_to_f32(uint f24) {
  f24 &= 0xFFFFFFu;
  if (f24 == 0u) return 0.0;
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20;
  if (exponent == 0u) {           // denormal: same normalization as k_20e4
    uint lz = clz(mantissa) - 11u;
    exponent = 1u - lz;
    mantissa = (mantissa << lz) & 0xFFFFFu;
  }
  return as_type<float>(((exponent + 112u) << 23) | (mantissa << 3));
}
inline uint xe_f32_to_f20e4(float f) {  // inverse; mirrors the d2c encode
  if (!(f > 0.0)) return 0u;
  uint u = as_type<uint>(f);
  if (u >= 0x40000000u) return 0xFFFFFFu;  // >= 2.0 saturates
  if (u < 0x38800000u) {                   // denormal in 20e4
    uint shift = min(113u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
  } else {
    u -= 0x38000000u;
  }
  return (u >> 3) & 0xFFFFFFu;
}
// Round-to-nearest-even f20e4 encode: same biasing as the truncating form,
// but the 3 dropped mantissa bits round to nearest with ties to even, and the
// increment saturates at the format maximum instead of wrapping. Mirrors
// xenos::Float32To20e4(f, /*round_to_nearest_even=*/true) — the stage-B
// reconcile arm and any future rounding-mode pack share ONE definition here.
inline uint xe_f32_to_f20e4_rne(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = as_type<uint>(f);
  if (u >= 0x40000000u) return 0xFFFFFFu;
  if (u < 0x38800000u) {
    uint shift = min(113u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
  } else {
    u -= 0x38000000u;
  }
  uint dropped = u & 7u;
  uint code = (u >> 3) & 0x1FFFFFFu;
  if (dropped > 4u || (dropped == 4u && (code & 1u))) ++code;
  return min(code, 0xFFFFFFu);
}
inline uint xe_f24_pack_mode(float f, uint rne) {
  return rne != 0u ? xe_f32_to_f20e4_rne(f) : xe_f32_to_f20e4(f);
}
// Stage B host-depth lane (implement-metal-depth-semantics). mode bits:
//   1 = pack writes the host-precision depth into the lane
//   2 = materialize reconciles from the lane
//   4 = the arm's f20e4 rounding is round-to-nearest-even
// The lane is a plain float array parallel to the canonical words (same idx
// space); validity is POINTWISE — the host value is used only if packing it
// under the arm's mode reproduces the guest word's depth bits, so a stale
// lane self-invalidates and no ownership metadata exists. Mirrored on the
// CPU by metal_host_depth_reconcile.h; the L1 kernel case pins the pair.
struct HostLaneParams { uint mode; };
inline float xe_host_depth_reconcile(uint word, float host_d, uint rne) {
  uint guest_d24 = xe_depth24(word);
  if (xe_f24_pack_mode(host_d * 2.0, rne) == guest_d24) return host_d;
  return xe_f20e4_to_f32(guest_d24) * 0.5;
}
// UNorm24 (kD24S8): the exact Xenia mapping. The numerator n24+(n24>>23) is an
// integer <= 2^24 (exactly representable in f32) and the *2^-24 only shifts the
// exponent, so EVERY decoded value is an exact f32 -- no near-1.0 collision.
inline float xe_unorm24_to_f32(uint n24) {
  n24 &= 0xFFFFFFu;
  return float(n24 + (n24 >> 23)) * (1.0 / 16777216.0);
}
inline uint xe_f32_to_unorm24(float d) {
  // EXACT inverse of xe_unorm24_to_f32: decode is (n24 + (n24>>23)) * 2^-24, so
  // d * 2^24 reconstructs the integer n24+b EXACTLY (power-of-two multiply, no
  // rounding), then remove the +b bias (b = 1 above the 0.5 boundary). Using
  // *2^24 (not *(2^24-1)) avoids the round-even tie that dropped near-boundary
  // values by 1.
  uint t = min(uint(rint(saturate(d) * 16777216.0)), 16777216u);
  return (t > 8388608u ? t - 1u : t) & 0xFFFFFFu;
}
// Decode a guest depth word's d24 to the internal host depth-attachment domain.
// float24 (fmt 2) spans guest [0,2): pack it to [0,1] with *0.5 so a Metal
// depth attachment holds the whole domain; unorm24 (fmt 1) is already [0,1].
// pack applies the inverse (*2 for float24).
inline float xe_depth_word_to_host(uint word, uint fmt) {
  uint d24 = xe_depth24(word);
  return fmt == 2u ? xe_f20e4_to_f32(d24) * 0.5 : xe_unorm24_to_f32(d24);
}
// Resolve snapshots are guest-visible numeric textures, not internal depth
// attachments. Decode them in the guest domain so a D24FS8 value g sampled
// after a resolve is still g rather than the host attachment encoding 0.5*g.
inline float xe_depth_word_to_guest(uint word, uint fmt) {
  uint d24 = xe_depth24(word);
  return fmt == 2u ? xe_f20e4_to_f32(d24) : xe_unorm24_to_f32(d24);
}
inline uint xe_host_to_depth_d24(float d, uint fmt) {
  return fmt == 2u ? xe_f32_to_f20e4(d * 2.0) : xe_f32_to_unorm24(d);
}
struct DepthVOut { float4 pos [[position]]; };
vertex DepthVOut xe_depth_vs(uint vid [[vertex_id]]) {
  DepthVOut o;
  float2 t = float2(float((vid << 1) & 2), float(vid & 2));
  o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
struct DepthFragOut { float depth [[depth(any)]]; };
fragment DepthFragOut materialize_depth_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]]) {
  uint2 gid = uint2(in.pos.xy);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthFragOut o;
  o.depth = xe_depth_word_to_host(w, sp.is_depth);  // is_depth: 2=float24,1=unorm24
  return o;
}
// Stencil materialize: no shader stencil export in Metal, so the guest stencil
// byte is rebuilt one bit-plane at a time -- this fragment discards where the
// source word's stencil bit `bit` is clear, and the caller's REPLACE state ORs
// the bit in where it survives (mirrors the c2d stencil rebuild). NOTE: the
// caller's depth pass must REPLACE stencil to 0 first, so a PARTIAL (scissored,
// loadAction:Load) write fully replaces instead of OR-ing over prior stencil.
fragment void materialize_sten_fs(DepthVOut in [[stage_in]],
                                  device const uint* canonical [[buffer(0)]],
                                  constant SurfaceParams& sp [[buffer(1)]],
                                  constant uint& bit [[buffer(2)]]) {
  uint2 gid = uint2(in.pos.xy);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  if ((xe_stencil(w) & bit) == 0u) discard_fragment();
}
// Pack emits the COMPLETE depth/stencil word: (d24 << 8) | stencil. Depth from
// the depth2d view, stencil from the X32_Stencil8 view of the same attachment.
kernel void pack_depth(depth2d<float, access::read> tex [[texture(0)]],
                      texture2d<uint, access::read> sten [[texture(1)]],
                      device uint* canonical [[buffer(0)]],
                      constant SurfaceParams& sp [[buffer(1)]],
                      uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, 0u);
  uint idx = off >> 2;
  if (idx >= sp.words) return;
  float d = tex.read(gid);
  uint s = sten.read(gid).r & 0xFFu;
  canonical[idx] =
      xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
}
// Per-sample 4x/2x depth+stencil: the same materialize/pack, but SAMPLE-AWARE.
// Materialize is sample-rate ([[sample_id]] forces per-sample execution) so each
// sample gets the canonical word at its own (pixel, sample) offset; pack reads
// depth2d_ms/texture2d_ms per sample. Reuses xe_depth_word_to_host /
// xe_host_to_depth_d24 / xe_edram_offset_bytes verbatim (the live path must too).
fragment DepthFragOut materialize_depth_ms_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    uint sid [[sample_id]]) {
  uint2 gid = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sid);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthFragOut o;
  o.depth = xe_depth_word_to_host(w, sp.is_depth);
  return o;
}
fragment void materialize_sten_ms_fs(DepthVOut in [[stage_in]],
                                     device const uint* canonical [[buffer(0)]],
                                     constant SurfaceParams& sp [[buffer(1)]],
                                     constant uint& bit [[buffer(2)]],
                                     uint sid [[sample_id]]) {
  uint2 gid = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sid);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  if ((xe_stencil(w) & bit) == 0u) discard_fragment();
}
kernel void pack_depth_ms(depth2d_ms<float, access::read> tex [[texture(0)]],
                         texture2d_ms<uint, access::read> sten [[texture(1)]],
                         device uint* canonical [[buffer(0)]],
                         constant SurfaceParams& sp [[buffer(1)]],
                         uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples()) return;
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  if (idx >= sp.words) return;
  float d = tex.read(uint2(gid.x, gid.y), gid.z);
  uint s = sten.read(uint2(gid.x, gid.y), gid.z).r & 0xFFu;
  canonical[idx] =
      xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
}
fragment DepthFragOut materialize_depth_ms_range_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device const float* lane [[buffer(4)]],
    uint sid [[sample_id]]) {
  uint2 gid = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sid);
  if (!xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthFragOut o;
  if ((hl.mode & 2u) && sp.is_depth == 2u && idx < sp.words) {
    o.depth = xe_host_depth_reconcile(w, lane[idx], (hl.mode >> 2u) & 1u);
  } else {
    o.depth = xe_depth_word_to_host(w, sp.is_depth);
  }
  return o;
}
fragment void materialize_sten_ms_range_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant uint& bit [[buffer(3)]],
    uint sid [[sample_id]]) {
  uint2 gid = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sid);
  if (!xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  if ((xe_stencil(w) & bit) == 0u) discard_fragment();
}
// Single-pass depth+stencil materialize: the fragment exports the stencil
// byte ([[stencil]]) so a REPLACE depth-stencil state writes the complete byte
// per sample, replacing the depth pass + eight bit-plane passes above.
struct DepthStencilFragOut {
  float depth [[depth(any)]];
  uint stencil [[stencil]];
};
fragment DepthStencilFragOut materialize_depth_sten_ms_range_fs(
    DepthVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device const float* lane [[buffer(4)]],
    uint sid [[sample_id]]) {
  uint2 gid = uint2(in.pos.xy);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, sid);
  if (!xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) {
    discard_fragment();
  }
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  uint w = (idx < sp.words) ? canonical[idx] : 0u;
  DepthStencilFragOut o;
  if ((hl.mode & 2u) && sp.is_depth == 2u && idx < sp.words) {
    o.depth = xe_host_depth_reconcile(w, lane[idx], (hl.mode >> 2u) & 1u);
  } else {
    o.depth = xe_depth_word_to_host(w, sp.is_depth);
  }
  o.stencil = xe_stencil(w);
  return o;
}
kernel void pack_depth_ms_range(
    depth2d_ms<float, access::read> tex [[texture(0)]],
    texture2d_ms<uint, access::read> sten [[texture(1)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device float* lane [[buffer(4)]],
    constant uint& row_start [[buffer(5)]],
    uint3 tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(tid.x, tid.y + row_start, tid.z);
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  if (idx >= sp.words) return;
  float d = tex.read(uint2(gid.x, gid.y), gid.z);
  uint s = sten.read(uint2(gid.x, gid.y), gid.z).r & 0xFFu;
  canonical[idx] =
      xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
  // Stage B: preserve the host-precision depth beside the packed word. Only
  // float24 (is_depth==2) carries extra precision worth a lane slot.
  if ((hl.mode & 1u) && sp.is_depth == 2u) lane[idx] = d;
}
// pack_depth_ms_range with one thread per pixel packing all of its samples
// (see xe_pixel_sample_word).
kernel void pack_depth_ms_range_px(
    depth2d_ms<float, access::read> tex [[texture(0)]],
    texture2d_ms<uint, access::read> sten [[texture(1)]],
    device uint* canonical [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant HostLaneParams& hl [[buffer(3)]],
    device float* lane [[buffer(4)]],
    constant uint& row_start [[buffer(5)]],
    uint2 tid [[thread_position_in_grid]]) {
  uint2 px = uint2(tid.x, tid.y + row_start);
  if (px.x >= tex.get_width() || px.y >= tex.get_height() ||
      !xe_authority_in_range(px.x, px.y, sp, 0u, range)) return;
  uint idx0 = xe_edram_offset_bytes(px.x, px.y, sp.base, sp.pitch, sp.msaa,
                                    sp.is_depth, 0u, 0u) >> 2;
  uint samples = tex.get_num_samples();
  for (uint ms = 0u; ms < samples; ++ms) {
    uint idx = xe_pixel_sample_word(
        idx0, xe_metal_sample_to_guest(sp.msaa, ms), 0u);
    if (idx >= sp.words) continue;
    float d = tex.read(px, ms);
    uint s = sten.read(px, ms).r & 0xFFu;
    canonical[idx] =
        xe_pack_depth_stencil(xe_host_to_depth_d24(d, sp.is_depth), s);
    if ((hl.mode & 1u) && sp.is_depth == 2u) lane[idx] = d;
  }
}
// Stage B provenance refresh: write the SELECTED host source's depth into
// the lane over the exact two-source intersection, without touching the
// canonical words. Run before a reconciling materialize so the lane carries
// the plan's provenance, not whatever pack last touched these slots.
kernel void lane_store_ms_range(
    depth2d_ms<float, access::read> tex [[texture(0)]],
    device float* lane [[buffer(0)]],
    constant SurfaceParams& sp [[buffer(1)]],
    constant AuthorityRangeParams& range [[buffer(2)]],
    constant uint& row_start [[buffer(7)]],
    uint3 xe_tid [[thread_position_in_grid]]) {
  // The dispatch starts at the range's first texture row (row_start).
  uint3 gid = uint3(xe_tid.x, xe_tid.y + row_start, xe_tid.z);
  if (sp.is_depth != 2u) return;
  uint guest_sample = xe_metal_sample_to_guest(sp.msaa, gid.z);
  if (gid.x >= tex.get_width() || gid.y >= tex.get_height() ||
      gid.z >= tex.get_num_samples() ||
      !xe_authority_in_range(gid.x, gid.y, sp, guest_sample, range)) return;
  uint off = xe_edram_offset_bytes(gid.x, gid.y, sp.base, sp.pitch, sp.msaa,
                                   sp.is_depth, 0u, guest_sample);
  uint idx = off >> 2;
  if (idx < sp.words) lane[idx] = tex.read(uint2(gid.x, gid.y), gid.z);
}

// Canonical resolve source. Destination formatting is performed by the Metal
// color attachment selected from RB_COPY_DEST_INFO; the existing writeback
// scatter then applies guest tiling, offsets, channel swap, and endian.
struct CanonicalResolveParams {
  SurfaceParams surface;
  uint source_x;
  uint source_y;
  uint dest_y;
  uint sample_select;
  uint color_format;
  uint dest_swap;
  uint dest_red_only;
  float exp_bias_scale;
  uint width;
  uint height;
  uint dest_format;
  uint dest_bpp_log2;
  uint dest_endian;
  uint raw_copy;
  // Stage B tiled scatter. When scatter_enable is 0 every field below is
  // ignored and the kernel writes the linear staging layout it always has, so
  // the flag-off path is byte-identical. When 1, `destination` is the shared
  // memory buffer (offset == guest physical address) and the kernel writes at
  // the guest tiled address instead, removing both the staging buffer and the
  // CPU scatter. 2D only: 3D/stacked destinations keep the CPU path.
  uint scatter_enable;
  uint dest_base;
  uint dest_pitch_aligned;
  uint dest_offset_x;
  uint dest_offset_y;
  uint dest_extent_bytes;
  // RB_COPY_DEST_INFO copy_dest_number: how fixed-point destination formats
  // are packed (see xe_resolve_pack_fixed).
  uint dest_number;
  // Integer-number-format destination (copy_dest_number 2/3) resolved into a
  // FLOAT snapshot texture: the component widths (8 bits each, r g b a) whose
  // 2^bits-1 divides the biased value, so the snapshot's normalized value is
  // the RAM texture's bits (the guest's integer fetch rescales by 2^bits-1).
  // 0 for packed destinations and fraction/float number formats.
  uint dest_int_bits;
};
// The draw resolution scale (host pixels per guest pixel) implied by the tile
// geometry, and the host pixel a resolve reads for a guest pixel: the
// representative (scale >> 1) pixel of its block, the same one the shader
// translator uses for memory export at scale. Identity at 1x.
inline uint2 xe_resolution_scale() {
  return uint2(XE_EDRAM_TILE_WIDTH_SAMPLES / 80u,
               XE_EDRAM_TILE_HEIGHT_SAMPLES / 16u);
}
inline uint2 xe_resolve_host_pixel(uint2 guest_pixel) {
  uint2 scale = xe_resolution_scale();
  return guest_pixel * scale + (scale >> 1u);
}
// The resolve read helpers below take HOST pixels: the guest-memory kernels
// pass the representative pixel (xe_resolve_host_pixel), the scaled snapshot
// passes every host pixel of the block.
inline float4 xe_resolve_read_color(device const uint* canonical,
                                    constant CanonicalResolveParams& p,
                                    uint2 pixel, uint sample) {
  uint idx = xe_edram_offset_bytes(
                 pixel.x, pixel.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, 0u, p.surface.fmt_log2, sample) >>
             2u;
  // ResolveInfo permits the EDRAM base plus the rectangle offset to wrap the
  // physical 2048-tile ring. Other canonical conversion kernels are dispatched
  // over already-split physical ranges, but resolves address the guest surface
  // directly and therefore must wrap here.
  idx %= p.surface.words;
  if (p.surface.fmt_log2 != 0u) {
    uint2 words = uint2(canonical[idx],
                        canonical[(idx + 1u) % p.surface.words]);
    return xe_unpack_color64(words, p.color_format);
  }
  uint word = idx < p.surface.words ? canonical[idx] : 0u;
  return xe_unpack_color32(word, p.color_format);
}
inline float4 xe_resolve_color_finish(float4 value,
                                      constant CanonicalResolveParams& p) {
  value *= p.exp_bias_scale;
  if (p.dest_int_bits != 0u) {
    uint4 bits = (uint4(p.dest_int_bits) >> uint4(0u, 8u, 16u, 24u)) & 0xFFu;
    value /= max(exp2(float4(bits)) - 1.0, 1.0);
  }
  if (p.dest_swap == 0u) {
    return value;
  }
  if (p.dest_red_only != 0u) {
    // k_8 + swap is A8 (Canary 2b3f0cb45, from XGCopySurface: D3D maps A8 to
    // k_8 with LOW_BLUE, so the swap selects the source alpha, not blue).
    // Formats without alpha read 1.0 before exponent bias. k_8_A and k_8_B
    // keep the red/blue exchange below.
    if (p.dest_format == 2u) {
      bool has_alpha = p.color_format == 0u || p.color_format == 2u ||
                       p.color_format == 3u || p.color_format == 5u ||
                       p.color_format == 7u;
      value.r = has_alpha ? value.a : p.exp_bias_scale;
      return value;
    }
    // The resolve red-only packing path selects blue on swap only when the
    // source format actually stores a blue component. R/RG sources retain R.
    if (p.color_format == 0u || p.color_format == 2u ||
        p.color_format == 3u || p.color_format == 5u ||
        p.color_format == 7u) {
      value.r = value.b;
    } else if (p.color_format == 15u) {
      // Swapped 64bpp red-only resolves advance to the second stored dword;
      // for 32_32_FLOAT that is the G component.
      value.r = value.g;
    }
    return value;
  }
  return value.bgra;
}
// Averaging the samples of an 8_8_8_8 target, as the console does it
// (docs/dev/xbox_hardware_tests.md, gpu_rounding: 112 of 112 cases, and no
// other width fits): each byte is widened to 10 bits by repeating its top
// bits, the sum is divided with truncation, and the top 8 bits are kept. A
// half-covered white edge is 0x7F, not 0x80. Other formats average as
// before. XE_METAL_RESOLVE_AVERAGE_FLOAT=1 restores the float average.
#ifndef XE_RESOLVE_AVERAGE_FLOAT
#define XE_RESOLVE_AVERAGE_FLOAT 0
#endif
inline float4 xe_resolve_sample_term(float4 color, uint color_format) {
  if (color_format != 0u || XE_RESOLVE_AVERAGE_FLOAT != 0) {
    return color;
  }
  float4 bytes = round(saturate(color) * 255.0);
  return bytes * 4.0 + floor(bytes * (1.0 / 64.0));
}
inline float4 xe_resolve_sample_mean(float4 sum, float count,
                                     uint color_format) {
  if (color_format != 0u || XE_RESOLVE_AVERAGE_FLOAT != 0) {
    return sum / count;
  }
  return floor(floor(sum / count) * 0.25) / 255.0;
}
inline float4 xe_resolve_color_value(device const uint* canonical,
                                     constant CanonicalResolveParams& p,
                                     uint2 source) {
  float4 value;
  switch (p.sample_select) {
    case 1u:
    case 2u:
    case 3u:
      value = xe_resolve_read_color(canonical, p, source, p.sample_select);
      break;
    case 4u:
      value = xe_resolve_sample_mean(
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 0u), p.color_format) +
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 1u), p.color_format),
          2.0, p.color_format);
      break;
    case 5u:
      value = xe_resolve_sample_mean(
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 2u), p.color_format) +
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 3u), p.color_format),
          2.0, p.color_format);
      break;
    case 6u:
      value = xe_resolve_sample_mean(
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 0u), p.color_format) +
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 1u), p.color_format) +
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 2u), p.color_format) +
          xe_resolve_sample_term(
              xe_resolve_read_color(canonical, p, source, 3u), p.color_format),
          4.0, p.color_format);
      break;
    default:
      value = xe_resolve_read_color(canonical, p, source, 0u);
      break;
  }
  return xe_resolve_color_finish(value, p);
}

// Guest resolve destination packing. Keep these switches in lockstep with
// pixel_formats.xesli XePack{16,32,64}bpp* and resolve_full_128bpp.xesli.
// Fixed-point pack by the destination number format (RB_COPY_DEST_INFO
// copy_dest_number): 0 unsigned fraction, 1 signed fraction, 2 unsigned
// integer, 3 signed integer; anything else packs as an unsigned fraction.
// A title that resolves a -32..32 fixed render target with exp_bias -5 into a
// signed 16-bit texture needs the signed pack: the unsigned one zeroes every
// negative value (Rise of the Tomb Raider's particle buffer, encoded as
// 2*(c-16), came back as a +16 wash over the whole frame).
inline uint xe_resolve_pack_fixed(float v, uint bits, uint num_format) {
  uint unsigned_max = (1u << bits) - 1u;
  uint packed;
  if (num_format == 1u) {
    float positive_max = float((1u << (bits - 1u)) - 1u);
    packed = uint(int(clamp(v, -1.0, 1.0) * positive_max +
                      (v >= 0.0 ? 0.5 : -0.5)));
  } else if (num_format == 2u) {
    packed = uint(clamp(v, 0.0, float(unsigned_max)) + 0.5);
  } else if (num_format == 3u) {
    float signed_min = -float(1u << (bits - 1u));
    float signed_max = float((1u << (bits - 1u)) - 1u);
    packed = uint(int(clamp(v, signed_min, signed_max) +
                      (v >= 0.0 ? 0.5 : -0.5)));
  } else {
    packed = uint(saturate(v) * float(unsigned_max) + 0.5);
  }
  return packed & unsigned_max;
}
inline uint xe_resolve_pack_5551(float4 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 5u, num_format) |
         (xe_resolve_pack_fixed(v.g, 5u, num_format) << 5u) |
         (xe_resolve_pack_fixed(v.b, 5u, num_format) << 10u) |
         (xe_resolve_pack_fixed(v.a, 1u, num_format) << 15u);
}
inline uint xe_resolve_pack_565(float3 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 5u, num_format) |
         (xe_resolve_pack_fixed(v.g, 6u, num_format) << 5u) |
         (xe_resolve_pack_fixed(v.b, 5u, num_format) << 11u);
}
inline uint xe_resolve_pack_556(float3 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 5u, num_format) |
         (xe_resolve_pack_fixed(v.g, 5u, num_format) << 5u) |
         (xe_resolve_pack_fixed(v.b, 6u, num_format) << 10u);
}
inline uint xe_resolve_pack_4444(float4 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 4u, num_format) |
         (xe_resolve_pack_fixed(v.g, 4u, num_format) << 4u) |
         (xe_resolve_pack_fixed(v.b, 4u, num_format) << 8u) |
         (xe_resolve_pack_fixed(v.a, 4u, num_format) << 12u);
}
inline uint xe_resolve_pack_8888(float4 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 8u, num_format) |
         (xe_resolve_pack_fixed(v.g, 8u, num_format) << 8u) |
         (xe_resolve_pack_fixed(v.b, 8u, num_format) << 16u) |
         (xe_resolve_pack_fixed(v.a, 8u, num_format) << 24u);
}
inline uint xe_resolve_pack_1010102(float4 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 10u, num_format) |
         (xe_resolve_pack_fixed(v.g, 10u, num_format) << 10u) |
         (xe_resolve_pack_fixed(v.b, 10u, num_format) << 20u) |
         (xe_resolve_pack_fixed(v.a, 2u, num_format) << 30u);
}
inline uint xe_resolve_pack_111110(float3 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 11u, num_format) |
         (xe_resolve_pack_fixed(v.g, 11u, num_format) << 11u) |
         (xe_resolve_pack_fixed(v.b, 10u, num_format) << 22u);
}
inline uint xe_resolve_pack_101111(float3 v, uint num_format) {
  return xe_resolve_pack_fixed(v.r, 10u, num_format) |
         (xe_resolve_pack_fixed(v.g, 11u, num_format) << 10u) |
         (xe_resolve_pack_fixed(v.b, 11u, num_format) << 21u);
}
inline uint xe_resolve_pack_16(float v, uint num_format) {
  return xe_resolve_pack_fixed(v, 16u, num_format);
}
inline uint xe_resolve_pack_16_16(float2 v, uint num_format) {
  return xe_resolve_pack_fixed(v.x, 16u, num_format) |
         (xe_resolve_pack_fixed(v.y, 16u, num_format) << 16u);
}
inline uint2 xe_resolve_pack_16_16_16_16(float4 v, uint num_format) {
  return uint2(xe_resolve_pack_fixed(v.x, 16u, num_format) |
                   (xe_resolve_pack_fixed(v.y, 16u, num_format) << 16u),
               xe_resolve_pack_fixed(v.z, 16u, num_format) |
                   (xe_resolve_pack_fixed(v.w, 16u, num_format) << 16u));
}
inline ushort xe_resolve_pack_16bpp(float4 v, uint format, uint num_format) {
  switch (format) {
    case 3u: return ushort(xe_resolve_pack_5551(v, num_format));
    case 4u: return ushort(xe_resolve_pack_565(v.rgb, num_format));
    case 5u: return ushort(xe_resolve_pack_556(v.rgb, num_format));
    case 10u:
      return ushort(xe_resolve_pack_fixed(v.r, 8u, num_format) |
                    (xe_resolve_pack_fixed(v.g, 8u, num_format) << 8u));
    case 15u: return ushort(xe_resolve_pack_4444(v, num_format));
    case 24u: return ushort(xe_resolve_pack_16(v.r, num_format));
    default: return ushort(xe_f32_to_f16_ext(v.r));
  }
}
inline uint xe_resolve_pack_32bpp(float4 v, uint format, uint num_format) {
  switch (format) {
    case 6u:
    case 14u:
    case 50u: return xe_resolve_pack_8888(v, num_format);
    case 7u:
    case 54u: return xe_resolve_pack_1010102(v, num_format);
    case 16u:
    case 55u: return xe_resolve_pack_111110(v.rgb, num_format);
    case 17u:
    case 56u: return xe_resolve_pack_101111(v.rgb, num_format);
    case 13u:
    case 25u: return xe_resolve_pack_16_16(v.rg, num_format);
    case 31u:
      return xe_f32_to_f16_ext(v.r) | (xe_f32_to_f16_ext(v.g) << 16u);
    default: return as_type<uint>(v.r);
  }
}
inline uint2 xe_resolve_pack_64bpp(float4 v, uint format, uint num_format) {
  switch (format) {
    case 21u:
    case 26u: return xe_resolve_pack_16_16_16_16(v, num_format);
    case 32u:
      return uint2(xe_f32_to_f16_ext(v.r) | (xe_f32_to_f16_ext(v.g) << 16u),
                   xe_f32_to_f16_ext(v.b) | (xe_f32_to_f16_ext(v.a) << 16u));
    default: return as_type<uint2>(v.rg);
  }
}
inline uint xe_resolve_endian32(uint v, uint endian) {
  if (endian == 1u || endian == 2u) {
    v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
inline uint2 xe_resolve_endian64(uint2 v, uint endian) {
  if (endian == 4u) {
    v = v.yx;
    endian = 2u;
  }
  return uint2(xe_resolve_endian32(v.x, endian),
               xe_resolve_endian32(v.y, endian));
}
inline uint4 xe_resolve_endian128(uint4 v, uint endian) {
  if (endian == 5u) {
    v = v.wzyx;
    endian = 2u;
  }
  uint2 lo = xe_resolve_endian64(v.xy, endian);
  uint2 hi = xe_resolve_endian64(v.zw, endian);
  return uint4(lo, hi);
}
// GUEST TEXTURE TILING (Stage B of implement-metal-gpu-visible-resolve-authority).
// Direct port of texture_address::TiledCombine / Tiled2D / Tiled3D. The CPU
// writeback scatter is the byte-exact oracle: these must agree with it for
// every (x, y, z, pitch_aligned, height_aligned, bpp) the resolve path can
// produce, which the [l1][metal][scatter] conformance cases assert.
//
// Ported verbatim rather than re-derived: the bank/pipe/y_lsb bit interleave
// is not reconstructible from the shape of the data, and a "cleaner" version
// that disagrees in one bit scrambles every tiled fetch of the destination.
inline uint xe_tiled_combine(uint outer_inner_bytes, uint bank, uint pipe,
                             uint y_lsb) {
  return ((y_lsb << 4u) | (pipe << 6u) | (bank << 11u)) |
         (outer_inner_bytes & 0xFu) |
         (((outer_inner_bytes >> 4u) & 0x1u) << 5u) |
         (((outer_inner_bytes >> 5u) & 0x7u) << 8u) |
         ((outer_inner_bytes >> 8u) << 12u);
}
inline uint xe_tiled2d_offset(uint x, uint y, uint pitch_aligned,
                              uint bytes_per_block_log2) {
  uint outer_blocks = (((y >> 5u) * (pitch_aligned >> 5u)) + (x >> 5u)) << 6u;
  uint inner_blocks = (((y >> 1u) & 0x7u) << 3u) | (x & 0x7u);
  uint outer_inner_bytes = (outer_blocks | inner_blocks)
                           << bytes_per_block_log2;
  uint bank = (y >> 4u) & 0x1u;
  uint pipe = ((x >> 3u) & 0x3u) ^ (((y >> 3u) & 0x1u) << 1u);
  return xe_tiled_combine(outer_inner_bytes, bank, pipe, y & 1u);
}

inline uint4 xe_resolve_raw_swap(uint4 value,
                                 constant CanonicalResolveParams& p) {
  if (p.dest_swap != 0u) {
    if (p.surface.fmt_log2 == 0u) {
      if (p.color_format == 0u) {
        value.x = (value.x & 0xFF00FF00u) |
                  ((value.x & 0xFFu) << 16u) |
                  ((value.x >> 16u) & 0xFFu);
      } else if (p.color_format == 2u) {
        value.x = (value.x & 0xC00FFC00u) |
                  ((value.x & 0x3FFu) << 20u) |
                  ((value.x >> 20u) & 0x3FFu);
      }
    } else if (p.color_format == 7u) {
      ushort4 h = as_type<ushort4>(value.xy);
      value.xy = as_type<uint2>(h.bgra);
    }
  }
  return value;
}
inline uint4 xe_resolve_read_raw(device const uint* canonical,
                                 constant CanonicalResolveParams& p,
                                 uint2 source) {
  uint sample = min(p.sample_select, 3u);
  uint idx = xe_edram_offset_bytes(
                 source.x, source.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, 0u, p.surface.fmt_log2, sample) >> 2u;
  idx %= p.surface.words;
  uint4 value = uint4(canonical[idx], 0u, 0u, 0u);
  if (p.surface.fmt_log2 != 0u) {
    value.y = canonical[(idx + 1u) % p.surface.words];
  }
  return xe_resolve_raw_swap(value, p);
}

inline void xe_resolve_store_raw(device uchar* destination, uint byte_offset,
                                 uint4 raw,
                                 constant CanonicalResolveParams& p) {
  if (p.dest_bpp_log2 == 2u) {
    *reinterpret_cast<device uint*>(destination + byte_offset) =
        xe_resolve_endian32(raw.x, p.dest_endian);
  } else {
    *reinterpret_cast<device uint2*>(destination + byte_offset) =
        xe_resolve_endian64(raw.xy, p.dest_endian);
  }
}
inline void xe_resolve_store_value(device uchar* destination,
                                   uint byte_offset, float4 value,
                                   constant CanonicalResolveParams& p) {
  switch (p.dest_bpp_log2) {
    case 0u:
      destination[byte_offset] =
          uchar(xe_resolve_pack_fixed(value.r, 8u, p.dest_number));
      break;
    case 1u: {
      ushort packed =
          xe_resolve_pack_16bpp(value, p.dest_format, p.dest_number);
      if (p.dest_endian == 1u) packed = ushort((packed << 8u) | (packed >> 8u));
      *reinterpret_cast<device ushort*>(destination + byte_offset) = packed;
      break;
    }
    case 2u:
      *reinterpret_cast<device uint*>(destination + byte_offset) =
          xe_resolve_endian32(
              xe_resolve_pack_32bpp(value, p.dest_format, p.dest_number),
              p.dest_endian);
      break;
    case 3u:
      *reinterpret_cast<device uint2*>(destination + byte_offset) =
          xe_resolve_endian64(
              xe_resolve_pack_64bpp(value, p.dest_format, p.dest_number),
              p.dest_endian);
      break;
    default:
      *reinterpret_cast<device uint4*>(destination + byte_offset) =
          xe_resolve_endian128(as_type<uint4>(value), p.dest_endian);
      break;
  }
}
// The guest words xe_resolve_store_value / xe_resolve_store_raw write for a
// 32bpp (.x) or 64bpp (.xy) destination.
inline uint2 xe_resolve_guest_words(float4 value,
                                    constant CanonicalResolveParams& p) {
  if (p.dest_bpp_log2 == 2u) {
    return uint2(xe_resolve_endian32(
                     xe_resolve_pack_32bpp(value, p.dest_format, p.dest_number),
                     p.dest_endian),
                 0u);
  }
  return xe_resolve_endian64(
      xe_resolve_pack_64bpp(value, p.dest_format, p.dest_number),
      p.dest_endian);
}
inline uint2 xe_resolve_guest_raw_words(uint4 raw,
                                        constant CanonicalResolveParams& p) {
  if (p.dest_bpp_log2 == 2u) {
    return uint2(xe_resolve_endian32(raw.x, p.dest_endian), 0u);
  }
  return xe_resolve_endian64(raw.xy, p.dest_endian);
}
inline void xe_resolve_store_words(device uchar* destination, uint byte_offset,
                                   uint2 words,
                                   constant CanonicalResolveParams& p) {
  if (p.dest_bpp_log2 == 2u) {
    *reinterpret_cast<device uint*>(destination + byte_offset) = words.x;
  } else {
    *reinterpret_cast<device uint2*>(destination + byte_offset) = words;
  }
}
// Resolve forwarding (XE_METAL_RESOLVE_FORWARD): the *_fwd scatter kernels
// also write, into the shared texture cache's texture of the destination, the
// texel that cache's load would make of the guest words, so the texture does
// not reload from the buffer. The loads apply the fetch endianness to each
// guest dword (texture_load_32bpb / _64bpb / _32bpb_direct) and store the
// result's bits: RGBA8 bytes (kind 1), four halves (kind 2), one dword
// (kind 3). The texture is bound through an unsigned integer view of the same
// size, so every bit pattern (NaN, denormal) is stored as the load stores it.
// `texel` is the destination's; the texture's row origin_y is the
// destination's row 0 (a resolve strip into a taller texture).
struct XeResolveForward {
  uint kind;
  uint endian;
  uint width;
  uint height;
  uint origin_y;
};
inline void xe_resolve_forward_store(texture2d<uint, access::write> texture,
                                     constant XeResolveForward& f,
                                     uint2 texel, uint2 guest) {
  texel.y += f.origin_y;
  if (texel.x >= f.width || texel.y >= f.height) {
    return;
  }
  uint w0 = xe_resolve_endian32(guest.x, f.endian);
  if (f.kind == 1u) {
    texture.write(uint4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, (w0 >> 16u) & 0xFFu,
                        w0 >> 24u),
                  texel);
  } else if (f.kind == 2u) {
    uint w1 = xe_resolve_endian32(guest.y, f.endian);
    texture.write(uint4(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u),
                  texel);
  } else {
    texture.write(uint4(w0, 0u, 0u, 0u), texel);
  }
}
kernel void resolve_canonical_color_packed(
    device const uint* canonical [[buffer(0)]],
    device uchar* destination [[buffer(1)]],
    constant CanonicalResolveParams& p [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) return;
  // Guest memory takes the representative host pixel of each guest pixel.
  uint2 source =
      xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));
  uint byte_offset = (gid.y * p.width + gid.x) << p.dest_bpp_log2;
  if (p.scatter_enable != 0u) {
    // Same texel, guest tiled address. Mirrors the CPU scatter exactly: the
    // destination offsets are added before tiling, the bpp comes from
    // dest_bpp_log2, and an out-of-extent texel is DROPPED rather than
    // clamped (writing it would corrupt a neighbouring guest allocation).
    uint off = xe_tiled2d_offset(p.dest_offset_x + gid.x,
                                 p.dest_offset_y + gid.y,
                                 p.dest_pitch_aligned, p.dest_bpp_log2);
    uint bpp = 1u << p.dest_bpp_log2;
    if (p.dest_extent_bytes != 0u && off + bpp > p.dest_extent_bytes) {
      return;
    }
    byte_offset = p.dest_base + off;
  }
  if (p.raw_copy != 0u) {
    xe_resolve_store_raw(destination, byte_offset,
                         xe_resolve_read_raw(canonical, p, source), p);
    return;
  }
  xe_resolve_store_value(destination, byte_offset,
                         xe_resolve_color_value(canonical, p, source), p);
}
kernel void resolve_canonical_color_packed_fwd(
    device const uint* canonical [[buffer(0)]],
    device uchar* destination [[buffer(1)]],
    constant CanonicalResolveParams& p [[buffer(2)]],
    constant XeResolveForward& f [[buffer(5)]],
    texture2d<uint, access::write> forward [[texture(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) return;
  uint2 source =
      xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));
  uint2 texel = uint2(p.dest_offset_x + gid.x, p.dest_offset_y + gid.y);
  uint off = xe_tiled2d_offset(texel.x, texel.y, p.dest_pitch_aligned,
                               p.dest_bpp_log2);
  if (p.dest_extent_bytes != 0u &&
      off + (1u << p.dest_bpp_log2) > p.dest_extent_bytes) {
    return;
  }
  uint2 words =
      p.raw_copy != 0u
          ? xe_resolve_guest_raw_words(
                xe_resolve_read_raw(canonical, p, source), p)
          : xe_resolve_guest_words(
                xe_resolve_color_value(canonical, p, source), p);
  xe_resolve_store_words(destination, p.dest_base + off, words, p);
  xe_resolve_forward_store(forward, f, texel, words);
}
// The snapshot is at the draw resolution scale: each of its host pixels copies
// the same host pixel of the source block (dest/source/width/height are guest
// pixels). Identity at 1x.
inline uint2 xe_resolve_snapshot_source(constant CanonicalResolveParams& p,
                                        uint2 host_dest, uint2 dest) {
  uint2 scale = xe_resolution_scale();
  return uint2(p.source_x + dest.x, p.source_y + dest.y - p.dest_y) * scale +
         host_dest % scale;
}
fragment float4 resolve_canonical_color_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant CanonicalResolveParams& p [[buffer(1)]]) {
  uint2 host_dest = uint2(in.pos.xy);
  uint2 dest = host_dest / xe_resolution_scale();
  if (dest.x >= p.width || dest.y < p.dest_y ||
      dest.y - p.dest_y >= p.height) {
    discard_fragment();
  }
  return xe_resolve_color_value(canonical, p,
                                xe_resolve_snapshot_source(p, host_dest, dest));
}
fragment float4 resolve_canonical_depth_fs(
    ColorVOut in [[stage_in]],
    device const uint* canonical [[buffer(0)]],
    constant CanonicalResolveParams& p [[buffer(1)]]) {
  uint2 host_dest = uint2(in.pos.xy);
  uint2 dest = host_dest / xe_resolution_scale();
  if (dest.x >= p.width || dest.y < p.dest_y ||
      dest.y - p.dest_y >= p.height) {
    discard_fragment();
  }
  uint2 source = xe_resolve_snapshot_source(p, host_dest, dest);
  uint idx = xe_edram_offset_bytes(
                 source.x, source.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, p.surface.is_depth, 0u,
                 p.sample_select) >>
             2u;
  uint word = canonical[idx % p.surface.words];
  return float4(xe_depth_word_to_guest(word, p.surface.is_depth), 0.0, 0.0,
                0.0);
}
// Compute twins of the two snapshot fragment functions above
// (XE_METAL_SNAPSHOT_COMPUTE): one thread per host pixel of the clipped
// destination rectangle, offset by `origin` (host pixels), the same
// destination->source mapping and per-pixel value, written straight into the
// snapshot instead of drawing a fullscreen triangle into a render pass that
// loads and stores the whole snapshot texture.
kernel void resolve_canonical_color_cs(
    device const uint* canonical [[buffer(0)]],
    constant CanonicalResolveParams& p [[buffer(1)]],
    constant uint2& origin [[buffer(2)]],
    texture2d_array<float, access::write> snapshot [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  uint2 host_dest = origin + gid;
  uint2 dest = host_dest / xe_resolution_scale();
  if (dest.x >= p.width || dest.y < p.dest_y ||
      dest.y - p.dest_y >= p.height) {
    return;
  }
  snapshot.write(xe_resolve_color_value(
                     canonical, p,
                     xe_resolve_snapshot_source(p, host_dest, dest)),
                 host_dest, 0u);
}
kernel void resolve_canonical_depth_cs(
    device const uint* canonical [[buffer(0)]],
    constant CanonicalResolveParams& p [[buffer(1)]],
    constant uint2& origin [[buffer(2)]],
    texture2d_array<float, access::write> snapshot [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  uint2 host_dest = origin + gid;
  uint2 dest = host_dest / xe_resolution_scale();
  if (dest.x >= p.width || dest.y < p.dest_y ||
      dest.y - p.dest_y >= p.height) {
    return;
  }
  uint2 source = xe_resolve_snapshot_source(p, host_dest, dest);
  uint idx = xe_edram_offset_bytes(
                 source.x, source.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, p.surface.is_depth, 0u,
                 p.sample_select) >>
             2u;
  uint word = canonical[idx % p.surface.words];
  snapshot.write(float4(xe_depth_word_to_guest(word, p.surface.is_depth), 0.0,
                        0.0, 0.0),
                 host_dest, 0u);
}
kernel void resolve_canonical_depth_raw(
    device const uint* canonical [[buffer(0)]],
    device uint* destination [[buffer(1)]],
    constant CanonicalResolveParams& p [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) {
    return;
  }
  uint2 source =
      xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));
  uint idx = xe_edram_offset_bytes(
                 source.x, source.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, p.surface.is_depth, 0u, p.sample_select) >>
             2u;
  // Depth resolve RAM is a raw copy. Preserve the selected physical D24S8 or
  // D24FS8 word, including stencil, while the separate R32F snapshot above is
  // the decoded sampling view used by Metal shaders.
  uint word = canonical[idx % p.surface.words];
  if (p.scatter_enable != 0u) {
    // Stage B for depth (optimize-metal-gpu-resident-resolve-coherence):
    // write the guest-endian word at its guest tiled address in the shared
    // buffer, exactly what the CPU drain did for the raw staging word.
    uint off = xe_tiled2d_offset(p.dest_offset_x + gid.x,
                                 p.dest_offset_y + gid.y,
                                 p.dest_pitch_aligned, 2u);
    if (p.dest_extent_bytes != 0u && off + 4u > p.dest_extent_bytes) {
      return;
    }
    device uchar* bytes = reinterpret_cast<device uchar*>(destination);
    *reinterpret_cast<device uint*>(bytes + p.dest_base + off) =
        xe_resolve_endian32(word, p.dest_endian);
    return;
  }
  destination[gid.y * p.width + gid.x] = word;
}
kernel void resolve_canonical_depth_raw_fwd(
    device const uint* canonical [[buffer(0)]],
    device uint* destination [[buffer(1)]],
    constant CanonicalResolveParams& p [[buffer(2)]],
    constant XeResolveForward& f [[buffer(5)]],
    texture2d<uint, access::write> forward [[texture(2)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) {
    return;
  }
  uint2 source =
      xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));
  uint idx = xe_edram_offset_bytes(
                 source.x, source.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, p.surface.is_depth, 0u, p.sample_select) >>
             2u;
  uint word = canonical[idx % p.surface.words];
  uint2 texel = uint2(p.dest_offset_x + gid.x, p.dest_offset_y + gid.y);
  uint off = xe_tiled2d_offset(texel.x, texel.y, p.dest_pitch_aligned, 2u);
  if (p.dest_extent_bytes != 0u && off + 4u > p.dest_extent_bytes) {
    return;
  }
  uint guest = xe_resolve_endian32(word, p.dest_endian);
  device uchar* bytes = reinterpret_cast<device uchar*>(destination);
  *reinterpret_cast<device uint*>(bytes + p.dest_base + off) = guest;
  xe_resolve_forward_store(forward, f, texel, uint2(guest, 0u));
}
#define XE_COMMA ,
// Direct colour resolves (XE_METAL_DIRECT_COLOR_RESOLVE): the canonical
// colour resolve kernels with the canonical word replaced by the word the pack
// would have written from the open 32bpp attachment. The canonical slot of a
// resolve read (pixel, sample) is inverted to the attachment's (pixel, guest
// sample) exactly as xe_edram_offset_bytes forms it (MSAA shift, then the
// sample's x/y sub-offset), and the guest sample to the host one with
// xe_metal_sample_to_guest (its own inverse). The caller proves the attachment
// holds the newest value of every tile, shares the resolve's base and pitch,
// and contains every pixel read (no ring wrap). Pixels are host pixels: at a
// draw resolution scale the representative pixel xe_resolve_host_pixel picks.
inline uint2 xe_direct_slot(constant CanonicalResolveParams& p, uint2 pixel,
                            uint sample, thread uint& guest_sample) {
  uint2 shifts = xe_edram_msaa_shifts(p.surface.msaa);
  uint2 s = (pixel << shifts) + ((uint2(sample, sample) >> uint2(1u, 0u)) & 1u);
  uint2 low = s & ((uint2(1u) << shifts) - 1u);
  guest_sample = (low.x << 1u) | low.y;
  return s >> shifts;
}
inline uint xe_direct_word_1x(texture2d<float, access::read> t,
                              constant CanonicalResolveParams& p,
                              uint rt_format, uint2 pixel, uint sample) {
  uint guest_sample;
  uint2 px = xe_direct_slot(p, pixel, sample, guest_sample);
  return xe_pack_color32_working(t.read(px), rt_format);
}
inline uint xe_direct_word_ms(texture2d_ms<float, access::read> t,
                              constant CanonicalResolveParams& p,
                              uint rt_format, uint2 pixel, uint sample) {
  uint guest_sample;
  uint2 px = xe_direct_slot(p, pixel, sample, guest_sample);
  return xe_pack_color32_working(
      t.read(px, xe_metal_sample_to_guest(p.surface.msaa, guest_sample)),
      rt_format);
}
#define XE_DIRECT_COLOR_KERNELS(SUFFIX, TEX_T)                                 \
  inline float4 xe_direct_read_color_##SUFFIX(                                 \
      TEX_T t, constant CanonicalResolveParams& p, uint rt_format,             \
      uint2 pixel, uint sample) {                                              \
    return xe_unpack_color32(                                                  \
        xe_direct_word_##SUFFIX(t, p, rt_format, pixel, sample),               \
        p.color_format);                                                       \
  }                                                                            \
  inline float4 xe_direct_color_value_##SUFFIX(                                \
      TEX_T t, constant CanonicalResolveParams& p, uint rt_format,             \
      uint2 source) {                                                          \
    float4 value;                                                              \
    switch (p.sample_select) {                                                 \
      case 1u:                                                                 \
      case 2u:                                                                 \
      case 3u:                                                                 \
        value = xe_direct_read_color_##SUFFIX(t, p, rt_format, source,         \
                                              p.sample_select);                \
        break;                                                                 \
      case 4u: \
        value = xe_resolve_sample_mean( \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 0u), p.color_format) + \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 1u), p.color_format), \
            2.0, p.color_format); \
        break; \
      case 5u: \
        value = xe_resolve_sample_mean( \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 2u), p.color_format) + \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 3u), p.color_format), \
            2.0, p.color_format); \
        break; \
      case 6u: \
        value = xe_resolve_sample_mean( \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 0u), p.color_format) + \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 1u), p.color_format) + \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 2u), p.color_format) + \
            xe_resolve_sample_term(xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 3u), p.color_format), \
            4.0, p.color_format); \
        break; \
      default:                                                                 \
        value = xe_direct_read_color_##SUFFIX(t, p, rt_format, source, 0u);    \
        break;                                                                 \
    }                                                                          \
    return xe_resolve_color_finish(value, p);                                  \
  }                                                                            \
  kernel void resolve_direct_color_cs_##SUFFIX(                                \
      TEX_T source_texture [[texture(1)]],                                     \
      constant CanonicalResolveParams& p [[buffer(1)]],                        \
      constant uint2& origin [[buffer(2)]],                                    \
      constant uint& rt_format [[buffer(3)]],                                  \
      texture2d_array<float, access::write> snapshot [[texture(0)]],           \
      uint2 gid [[thread_position_in_grid]]) {                                 \
    uint2 host_dest = origin + gid;                                            \
    uint2 dest = host_dest / xe_resolution_scale();                            \
    if (dest.x >= p.width || dest.y < p.dest_y ||                              \
        dest.y - p.dest_y >= p.height) {                                       \
      return;                                                                  \
    }                                                                          \
    snapshot.write(xe_direct_color_value_##SUFFIX(                             \
                       source_texture, p, rt_format,                           \
                       xe_resolve_snapshot_source(p, host_dest, dest)),        \
                   host_dest, 0u);                                             \
  }                                                                            \
  kernel void resolve_direct_color_packed_##SUFFIX(                            \
      TEX_T source_texture [[texture(0)]],                                     \
      device uchar* destination [[buffer(1)]],                                 \
      constant CanonicalResolveParams& p [[buffer(2)]],                        \
      constant uint& rt_format [[buffer(3)]],                                  \
      uint2 gid [[thread_position_in_grid]]) {                                 \
    if (gid.x >= p.width || gid.y >= p.height) return;                         \
    uint2 source =                                                             \
        xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));  \
    uint byte_offset = (gid.y * p.width + gid.x) << p.dest_bpp_log2;           \
    if (p.scatter_enable != 0u) {                                              \
      uint off = xe_tiled2d_offset(p.dest_offset_x + gid.x,                    \
                                   p.dest_offset_y + gid.y,                    \
                                   p.dest_pitch_aligned, p.dest_bpp_log2);     \
      uint bpp = 1u << p.dest_bpp_log2;                                        \
      if (p.dest_extent_bytes != 0u && off + bpp > p.dest_extent_bytes) {      \
        return;                                                                \
      }                                                                        \
      byte_offset = p.dest_base + off;                                         \
    }                                                                          \
    if (p.raw_copy != 0u) {                                                    \
      xe_resolve_store_raw(                                                    \
          destination, byte_offset,                                            \
          xe_resolve_raw_swap(                                                 \
              uint4(xe_direct_word_##SUFFIX(source_texture, p, rt_format,      \
                                            source,                            \
                                            min(p.sample_select, 3u)),         \
                    0u, 0u, 0u),                                               \
              p),                                                              \
          p);                                                                  \
      return;                                                                  \
    }                                                                          \
    xe_resolve_store_value(                                                    \
        destination, byte_offset,                                              \
        xe_direct_color_value_##SUFFIX(source_texture, p, rt_format, source),  \
        p);                                                                    \
  }                                                                            \
  kernel void resolve_direct_color_packed_fwd_##SUFFIX(                        \
      TEX_T source_texture [[texture(0)]],                                     \
      device uchar* destination [[buffer(1)]],                                 \
      constant CanonicalResolveParams& p [[buffer(2)]],                        \
      constant uint& rt_format [[buffer(3)]],                                  \
      constant XeResolveForward& f [[buffer(5)]],                              \
      texture2d<uint, access::write> forward [[texture(2)]],                  \
      uint2 gid [[thread_position_in_grid]]) {                                 \
    if (gid.x >= p.width || gid.y >= p.height) return;                         \
    uint2 source =                                                             \
        xe_resolve_host_pixel(uint2(p.source_x + gid.x, p.source_y + gid.y));  \
    uint2 texel = uint2(p.dest_offset_x + gid.x, p.dest_offset_y + gid.y);     \
    uint off = xe_tiled2d_offset(texel.x, texel.y, p.dest_pitch_aligned,       \
                                 p.dest_bpp_log2);                             \
    if (p.dest_extent_bytes != 0u &&                                           \
        off + (1u << p.dest_bpp_log2) > p.dest_extent_bytes) {                 \
      return;                                                                  \
    }                                                                          \
    uint2 words =                                                              \
        p.raw_copy != 0u                                                       \
            ? xe_resolve_guest_raw_words(                                      \
                  xe_resolve_raw_swap(                                         \
                      uint4(xe_direct_word_##SUFFIX(source_texture, p,         \
                                                    rt_format, source,         \
                                                    min(p.sample_select, 3u)), \
                            0u, 0u, 0u),                                       \
                      p),                                                      \
                  p)                                                           \
            : xe_resolve_guest_words(                                          \
                  xe_direct_color_value_##SUFFIX(source_texture, p, rt_format, \
                                                 source),                      \
                  p);                                                          \
    xe_resolve_store_words(destination, p.dest_base + off, words, p);          \
    xe_resolve_forward_store(forward, f, texel, words);                        \
  }
XE_DIRECT_COLOR_KERNELS(1x, texture2d<float XE_COMMA access::read>)
XE_DIRECT_COLOR_KERNELS(ms, texture2d_ms<float XE_COMMA access::read>)
// Canonical clear scatter. One gid is one host pixel and one physical MSAA
// sample; it writes the clear word(s) straight into the surface's own sample
// layout (including depth's 40-column swap via surface.is_depth), so a cleared
// canonical EDRAM matches what a resolve of the same surface would read back.
// source/width/height are guest pixels; the grid covers them at the draw
// resolution scale, so every host pixel of each guest pixel is cleared.
struct CanonicalClearParams {
  SurfaceParams surface;
  uint source_x;
  uint source_y;
  uint width;
  uint height;
  uint clear_lo;
  uint clear_hi;
};
// Resolve clear written straight into the colour attachment
// (reduce-edram-pass-traffic Lever 2): the value is exactly what
// materialize would produce from the canonical clear words, so the
// attachment holds the same texels without the canonical round trip.
// clear = (lo, hi, format, is_64bpp), lo/hi as resolve_canonical_clear writes
// them.
fragment float4 clear_color_attachment_fs(ColorVOut in [[stage_in]],
                                          constant uint4& clear
                                          [[buffer(0)]]) {
  return clear.w != 0u ? xe_unpack_color64(uint2(clear.x, clear.y), clear.z)
                       : xe_unpack_color32_working(clear.x, clear.z);
}
// The same clear written from compute into a single-sample attachment
// (ShaderWrite usage): only the rectangle's texels, no render pass load and
// store of the whole attachment. rect = (x, y, width, height) in host pixels.
kernel void clear_color_attachment_cs(
    texture2d<float, access::write> target [[texture(0)]],
    constant uint4& clear [[buffer(0)]],
    constant uint4& rect [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= rect.z || gid.y >= rect.w) {
    return;
  }
  float4 value = clear.w != 0u
                     ? xe_unpack_color64(uint2(clear.x, clear.y), clear.z)
                     : xe_unpack_color32_working(clear.x, clear.z);
  target.write(value, rect.xy + gid);
}
kernel void resolve_canonical_clear(
    device uint* canonical [[buffer(0)]],
    constant CanonicalClearParams& p [[buffer(1)]],
    uint3 gid [[thread_position_in_grid]]) {
  uint2 scale = xe_resolution_scale();
  if (gid.x >= p.width * scale.x || gid.y >= p.height * scale.y ||
      gid.z >= (1u << p.surface.msaa)) {
    return;
  }
  uint2 host = uint2(p.source_x, p.source_y) * scale + gid.xy;
  uint idx = xe_edram_offset_bytes(
                 host.x, host.y, p.surface.base, p.surface.pitch,
                 p.surface.msaa, p.surface.is_depth, p.surface.fmt_log2,
                 gid.z) >>
             2u;
  idx %= p.surface.words;
  canonical[idx] = p.clear_lo;
  if (p.surface.fmt_log2 != 0u) {
    canonical[(idx + 1u) % p.surface.words] = p.clear_hi;
  }
}
)MSL";

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_EDRAM_MSL_H_
