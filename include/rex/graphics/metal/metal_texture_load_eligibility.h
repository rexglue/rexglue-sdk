/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_LOAD_ELIGIBILITY_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_LOAD_ELIGIBILITY_H_

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "rex/graphics/texture_address.h"
#include "rex/graphics/metal/metal_shared_texture_format_table.h"
#include <rex/xenos/state.h>

// Pure (device-free) strict-RGBA8 GPU texture-load contract, proven byte-exact
// against the CPU untile across the recorded corpus (OpenSpec
// implement-metal-gpu-texture-loads, slices 3/4/5R): the eligibility
// predicate, the EXACT guest source range the 32bpb load shader reads, and
// the 256-aligned buffer-to-texture destination layout. The 5R landing
// decision replays the GPU load through the shared TextureCache seam
// (LoadTextureDataFromResidentMemoryImpl); this header carries the proven
// contract to that seam. Guarded by
// conformance/metal_texture_load_eligibility_test.cc — weakening a condition
// there fails the suite.

namespace rex {
namespace graphics {
namespace metal {

// ---- Strict first class: k_8_8_8_8, unsigned/gamma, tiled, 2D, base mip ----

// One shared predicate for every strict-RGBA8 GPU-load consumer.
// Every condition is deliberate; do NOT weaken to increase engagement.
// width/height/pitch_texels are the derived values the texture cache uses
// (pitch defaulted to align32(width) when the fetch pitch is zero).
inline bool IsStrictRgba8Eligible(const xenos::TextureFetch& fetch,
                                  bool is_signed, uint32_t width,
                                  uint32_t height, uint32_t pitch_texels) {
  // k_8_8_8_8 and its byte-identical alias k_8_8_8_8_AS_16_16_16_16 (fmt 50:
  // same bytes, same decode — the legacy FormatPlan maps both to one plan,
  // and JC2's temporal composite fetches its history buffer with the alias).
  // Value-changing aliases (k_8_8_8_8_A/AS_8_8_8_8 etc.) stay excluded.
  if (fetch.format != xenos::TextureFormat::k_8_8_8_8 &&
      fetch.format != xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16) {
    return false;
  }
  // Unsigned binding only (signed fetches are a separate SNORM-view
  // binding). Per-component signs may be kUnsigned or kGamma —
  // both sample the same unorm BYTES (gamma is a sampling-time transfer
  // curve in the translated shader, not an upload transform), and the
  // shipped exact-cover snapshot reader serves this class without
  // consulting component signs at all. kSigned/kUnsignedBiased components
  // change value interpretation classes and stay excluded.
  // (5R amendment, user-approved 2026-07-18: the entire live JC2
  // resolve-backed pool is gamma-signed; unsigned-only excluded it.)
  if (is_signed) {
    return false;
  }
  // kUnsignedBiased too: the same unorm bytes, biased by the translated
  // shader from the fetch constant's sign bits (see the signed predicate).
  auto sign_ok = [](xenos::TextureSign s) {
    return s == xenos::TextureSign::kUnsigned ||
           s == xenos::TextureSign::kGamma ||
           s == xenos::TextureSign::kUnsignedBiased;
  };
  if (!sign_ok(fetch.sign_x) || !sign_ok(fetch.sign_y) ||
      !sign_ok(fetch.sign_z) || !sign_ok(fetch.sign_w)) {
    return false;
  }
  if (!fetch.tiled) {
    return false;
  }
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    return false;
  }
  if (fetch.stacked) {
    return false;
  }
  if (fetch.mip_min_level != 0 || fetch.mip_max_level != 0) {
    return false;
  }
  if (!fetch.base_address || !width || !height || !pitch_texels) {
    return false;
  }
  return true;
}

// C1T-R route eligibility: the shared-subclass route additionally requires a
// nonzero fetch pitch — the shared TextureKey carries the RAW fetch pitch,
// so the legacy cache's align32(width) default for pitch==0 fetches is not
// representable there; such fetches stay on the legacy CPU path. This is a
// NARROWING for one consumer, never a weakening of the contract above.
inline bool IsStrictRgba8RouteEligible(
    const xenos::TextureFetch& fetch, bool is_signed, uint32_t width,
    uint32_t height, uint32_t pitch_texels) {
  return IsStrictRgba8Eligible(fetch, is_signed, width, height, pitch_texels) &&
         fetch.pitch != 0;
}

// ---- SIGNED-binding sibling (reopen-metal-gpu-visible-resolve-authority) ----
//
// A SIGNED k_8_8_8_8 binding samples the SAME guest bytes through an
// RGBA8Snorm VIEW (Xenos 8-bit signed texels are two's-complement — exactly
// host snorm bytes; the legacy plan's host_format_signed encodes the same
// equivalence). Deliberately a separate predicate so the strict class above
// is not weakened: every structural condition is identical, the differences
// are that the binding is signed and per-component signs may additionally be
// kSigned. kUnsignedBiased stays excluded everywhere — it is a value
// transform, not a reinterpreting view.
inline bool IsStrictRgba8SignedRouteEligible(
    const xenos::TextureFetch& fetch, uint32_t width,
    uint32_t height, uint32_t pitch_texels) {
  if (fetch.format != xenos::TextureFormat::k_8_8_8_8 &&
      fetch.format != xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16) {
    return false;
  }
  // kUnsignedBiased samples the same unorm bytes: the translator decodes the
  // bias in the shader from the fetch constant's sign bits (signs_temp in
  // dxbc_shader_translator_fetch.cc), on every path, so for the cache it is
  // kUnsigned. Excluding it sent Tomb Raider's 1280x720 / 640x360 RGBA8
  // post-process buffers to the CPU path, whose RAM reads forced 58
  // memexport drains and 25 ms of GPU waits a frame (2026-10-16).
  auto sign_ok = [](xenos::TextureSign) { return true; };
  if (!sign_ok(fetch.sign_x) || !sign_ok(fetch.sign_y) ||
      !sign_ok(fetch.sign_z) || !sign_ok(fetch.sign_w)) {
    return false;
  }
  if (!fetch.tiled || fetch.dimension != xenos::DataDimension::k2DOrStacked ||
      fetch.stacked) {
    return false;
  }
  // Single-mip shape ONLY (exactly the strict class's mip condition). A
  // mipped signed fetch served through a view of a single-level entry would
  // clamp every minified sample to level 0 — measured as a 125k-px SF4
  // regression when this predicate briefly admitted mipped shapes.
  if (fetch.mip_min_level != 0 || fetch.mip_max_level != 0) {
    return false;
  }
  if (!fetch.base_address || !width || !height || !pitch_texels) {
    return false;
  }
  return fetch.pitch != 0;
}

// ---- Mipped sibling class (OpenSpec 6.1) ----

// The MIPPED sibling of the strict class. Deliberately a separate predicate:
// IsStrictRgba8Eligible's single-mip rejection above is pinned by conformance
// and is NOT weakened, because the canonical single-mip route keeps depending
// on it exactly as promoted.
//
// Every non-mip condition is identical to the strict predicate (evaluated by
// masking the mip levels off and delegating, so the two can never drift), plus:
//   - mip_min_level == 0 EXPLICITLY. A nonzero min level means the guest
//     samples a shifted level set, which this slice does not express.
//   - mip_max_level > 0, i.e. there is actually a chain to load.
//   - a nonzero mip_address: the chain lives at its own guest pointer, and the
//     per-level plan reads levels 1..max from there.
//   - a nonzero fetch pitch, for the same reason the single-mip route requires
//     it (the shared TextureKey carries the raw pitch).
inline bool SharedRouteMinMipEnabled();
inline bool IsStrictRgba8MipRouteEligible(
    const xenos::TextureFetch& fetch, bool is_signed, uint32_t width,
    uint32_t height, uint32_t pitch_texels) {
  // Minimum level above 0: the same mips-only class as the generic route
  // (Red Dead gameplay, 2026-10-05: its 128x64 RGBA8 pyramids fetched one
  // level at a time were the four blocking consumer waits per frame; the
  // shared cache serves mips-only keys since 2026-10-04).
  if (fetch.mip_max_level == 0 ||
      fetch.mip_min_level > fetch.mip_max_level) {
    return false;
  }
  if (fetch.mip_min_level != 0 && !SharedRouteMinMipEnabled()) {
    return false;
  }
  if (!fetch.mip_address || !fetch.pitch) {
    return false;
  }
  // The RGBA4 -> RGBA8 expansion (non-Apple GPUs) serves mips unless
  // XE_METAL_RGBA4_EXPAND_MIPS=0 (see SharedRgba4ExpandMipsEnabled).
  if (const SharedTextureFormatEntry* entry =
          FindSharedTextureFormat(fetch.format)) {
    if (entry->load_shader == SharedLoadFamily::k16bpbRgba4ToRgba8 &&
        !SharedRgba4ExpandMipsEnabled()) {
      return false;
    }
  }
  xenos::TextureFetch as_base = fetch;
  as_base.mip_min_level = 0;
  as_base.mip_max_level = 0;
  return IsStrictRgba8Eligible(as_base, is_signed, width, height, pitch_texels);
}

// ---- Trace-record extent ----

// The guest byte extent the legacy cache records for a non-cube texture
// upload (watched_base_bytes in MetalTextureCache::GetTextureSlot) and the
// warm-entry trace sweep. Factored here as the documented single expression
// of that contract. (The C1T-R route needs no matching record: it disengages
// for captured frames, so the legacy path records the CPU-oracle read-set —
// the gate-C identical-ranges property holds by construction.)
// ---- Stage-1 widening (optimize-metal-gpu-resident-resolve-coherence):
// every format in metal_shared_texture_format_table.h, same shape rules as
// the strict RGBA8 class (tiled, 2D, not stacked, nonzero fetch pitch).
// Signed bindings are served as views of the same host texture, so the only
// sign exclusion is kUnsignedBiased (a value-changing class).
inline bool IsSharedRouteFormat(xenos::TextureFormat format) {
  return FindSharedTextureFormat(format) != nullptr;
}

// Stacked 2D textures (2D arrays, every slice at the level's array stride)
// load through the family loaders into a 2DArray host texture. Gears of War 3
// samples a stacked 16x16 RGBA8 texture in the draws that read its bloom
// chain: unrouted, it forced each such draw to wait for the GPU (5-7k waits a
// run). XE_METAL_TEX_ROUTE_STACKED=0 keeps stacked textures on the legacy
// cache.
inline bool SharedRouteStackedEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_STACKED");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

// Linear (untiled) 2D textures load through the family loaders with the
// upstream shaders' linear addressing (is_tiled bit clear). Rage streams its
// 16 MB DXT5 megatexture page cache and its 5:6:5 mip chains linear, and the
// legacy CPU untile of them was 11 ms/frame after the signed-twin fix.
// XE_METAL_TEX_ROUTE_LINEAR=0 keeps linear textures on the legacy cache.
inline bool SharedRouteLinearEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_LINEAR");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

// Cube maps load as six array slices of the upstream guest layout into a
// cube-array host texture (the translator samples cubes as texturecube_array).
// RE5 renders a 128x128 RGBA8 cube map with mips every frame; unrouted, the
// draw sampling it was a RAM-publication consumer (53 publications, 17 ms of
// CP per frame). XE_METAL_TEX_ROUTE_CUBE=0 keeps cubes on the legacy cache.
inline bool SharedRouteCubeEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_CUBE");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

// Volume textures on the GPU load route (the load shaders' 3D path; Tomb
// Raider's per-frame 32^3 colour LUT). XE_METAL_TEX_ROUTE_3D=0 keeps them on
// the legacy cache.
inline bool SharedRouteVolumeEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_3D");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

// 1D textures load through the family loaders as one linear row per level
// into a width x 1 2D host texture (2026-10-26). Before, the legacy cache
// served them base-only from a CPU untile. XE_METAL_TEX_ROUTE_1D=0 keeps
// them on the legacy cache.
inline bool SharedRoute1DEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_1D");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

inline bool IsSharedRouteShapeEligible(
    const xenos::TextureFetch& fetch, uint32_t width,
    uint32_t height, uint32_t pitch_texels) {
  const bool cube = fetch.dimension == xenos::DataDimension::kCube &&
                    SharedRouteCubeEnabled() && width == height;
  const bool volume = fetch.dimension == xenos::DataDimension::k3D &&
                      SharedRouteVolumeEnabled() && fetch.tiled;
  const bool one_d = fetch.dimension == xenos::DataDimension::k1D &&
                     SharedRoute1DEnabled() && !fetch.tiled &&
                     !fetch.packed_mips &&
                     width < xenos::kTexture2DCubeMaxWidthHeight;
  if ((!fetch.tiled && !SharedRouteLinearEnabled()) ||
      (fetch.dimension != xenos::DataDimension::k2DOrStacked && !cube &&
       !volume && !one_d) ||
      (fetch.stacked && !SharedRouteStackedEnabled())) {
    return false;
  }
  if (!fetch.base_address || !width || !height || !pitch_texels) {
    return false;
  }
  // 1D: the row pitch addresses nothing (one row), so zero is fine.
  return fetch.pitch != 0 || one_d;
}

// Single-mip class for any table format.
inline bool IsSharedRouteEligible(const xenos::TextureFetch& fetch,
                                  bool is_signed, uint32_t width,
                                  uint32_t height, uint32_t pitch_texels) {
  const SharedTextureFormatEntry* entry = FindSharedTextureFormat(fetch.format);
  if (!entry) {
    return false;
  }
  if (fetch.mip_min_level != 0 || fetch.mip_max_level != 0) {
    return false;
  }
  // Formats with a distinct snorm sibling serve signed bindings through that
  // view, so the component signs decide only which view a binding gets (a
  // kSigned component must never reach the unorm view, mirroring the
  // promoted RGBA8 class). Float formats and formats without a snorm sibling
  // (RGB10A2) use one host format for every sign, exactly as the legacy
  // plan does, so their component signs do not gate routing.
  const bool sign_selects_view = entry->host_format_signed != entry->host_format;
  if (sign_selects_view) {
    // kUnsignedBiased: see the strict signed predicate -- the bias is a
    // shader-side decode of the same bytes, not a texture class.
    auto sign_ok = [](xenos::TextureSign) { return true; };
    if (!sign_ok(fetch.sign_x) || !sign_ok(fetch.sign_y) ||
        !sign_ok(fetch.sign_z) || !sign_ok(fetch.sign_w)) {
      return false;
    }
    if (!is_signed) {
      auto unsigned_ok = [](xenos::TextureSign s) {
        return s == xenos::TextureSign::kUnsigned ||
               s == xenos::TextureSign::kGamma ||
               s == xenos::TextureSign::kUnsignedBiased;
      };
      if (!unsigned_ok(fetch.sign_x) || !unsigned_ok(fetch.sign_y) ||
          !unsigned_ok(fetch.sign_z) || !unsigned_ok(fetch.sign_w)) {
        return false;
      }
    }
  }
  return IsSharedRouteShapeEligible(fetch, width, height, pitch_texels);
}

// Mipped sibling: the chain has its own guest pointer and the base rules
// hold for the level-0 shape.
// A fetch whose minimum level is above 0 (Red Dead resolves into mips 1 and 2
// of its 4:4:4:4 pyramids and fetches each level alone) still loads the whole
// chain: the shared key ignores mip_min_level and the sampler clamps the LOD
// to [min, max] (D3D12 MinLOD/MaxLOD). XE_METAL_TEX_ROUTE_MINMIP=0 restores
// the base-level-only mip class.
inline bool SharedRouteMinMipEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_MINMIP");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_enabled;
}

inline bool IsSharedRouteMipEligible(const xenos::TextureFetch& fetch,
                                     bool is_signed, uint32_t width,
                                     uint32_t height, uint32_t pitch_texels) {
  if (fetch.mip_max_level == 0 ||
      fetch.mip_min_level > fetch.mip_max_level) {
    return false;
  }
  if (fetch.mip_min_level != 0 && !SharedRouteMinMipEnabled()) {
    return false;
  }
  if (!fetch.mip_address ||
      (!fetch.pitch && fetch.dimension != xenos::DataDimension::k1D)) {
    return false;
  }
  xenos::TextureFetch as_base = fetch;
  as_base.mip_min_level = 0;
  as_base.mip_max_level = 0;
  return IsSharedRouteEligible(as_base, is_signed, width, height, pitch_texels);
}

inline uint32_t ComputeWatchedBaseBytes(uint32_t pitch_blocks_aligned,
                                        uint32_t blocks_h, uint32_t bpb,
                                        uint32_t base,
                                        uint32_t physical_size) {
  uint64_t bytes = uint64_t(pitch_blocks_aligned) * blocks_h * bpb;
  uint64_t max_bytes = physical_size > base ? physical_size - base : 0;
  return uint32_t(std::min<uint64_t>(bytes, max_bytes));
}

// ---- Conservative source envelope ----

// The 32bpb load shader reads 16-byte groups at Tiled2D(4g, y) byte offsets
// (each 4 consecutive x blocks are byte-contiguous; the second uint4 of a
// thread is the g+1 group via the local-XOR step), for x groups covering the
// grid padded to the shader's 8-blocks-per-thread stride, and every y row
// (the y bounds guard is exact). The exact read set is
//   { [base + Tiled2D(4g, y), +16) : 4g < round_up(blocks_w, 8),
//                                    y < blocks_h }
// but TextureLoadSourceRange stores only the minimum/maximum ENVELOPE around
// that set. Tiled addressing may leave holes inside it. The envelope is still
// computed with the SAME shared tiled-address helper the CPU untile uses —
// never a pitch*height*bpb approximation — and is conservatively safe for
// RequestConsumedRange and hazard planning.
struct TextureLoadSourceRange {
  uint32_t begin = 0;  // Absolute guest byte address, inclusive.
  uint32_t end = 0;    // Absolute guest byte address, exclusive.
  bool valid = false;
};

// Origin-aware envelope calculation. The 32bpb direct shader dispatches
// ceil(w/8) threads along x, each covering 8 blocks as two 4-block groups, and
// reads Tiled2D(px + 4k, py + y) -- so for a packed sub-mip the read set starts
// AT the sub-tile origin, not at (0,0). Scanning from the origin avoids claiming
// the bytes of every larger level sharing the tail, though the returned min/max
// pair remains a conservative envelope rather than the exact group union.
//
// All intermediates are 64-bit and every value that reaches Tiled2D is proven
// to fit in int32, so a hostile or corrupt fetch cannot wrap into a bogus
// in-range span.
inline TextureLoadSourceRange ComputeStrictRgba8SourceRangeAtOrigin(
    uint32_t base, uint32_t px, uint32_t py, uint32_t blocks_w,
    uint32_t blocks_h, uint32_t pitch_blocks_aligned, uint32_t physical_size) {
  TextureLoadSourceRange r;
  if (!blocks_w || !blocks_h || !pitch_blocks_aligned) {
    return r;
  }
  uint64_t padded_w = (uint64_t(blocks_w) + 7u) & ~uint64_t(7u);
  uint64_t x_end = uint64_t(px) + padded_w;
  uint64_t y_end = uint64_t(py) + uint64_t(blocks_h);
  // Everything handed to Tiled2D must be a non-negative int32.
  if (x_end > uint64_t(INT32_MAX) || y_end > uint64_t(INT32_MAX) ||
      pitch_blocks_aligned > uint32_t(INT32_MAX)) {
    return r;
  }
  int64_t min_off = INT64_MAX;
  int64_t max_off = INT64_MIN;
  for (uint64_t y = py; y < y_end; ++y) {
    for (uint64_t x = px; x < x_end; x += 4) {
      int32_t off = texture_address::Tiled2D(int32_t(x), int32_t(y),
                                             pitch_blocks_aligned,
                                             /*bytes_per_block_log2=*/2);
      if (off < 0) {
        return r;
      }
      min_off = std::min<int64_t>(min_off, off);
      max_off = std::max<int64_t>(max_off, off);
    }
  }
  if (min_off > max_off) {
    return r;
  }
  uint64_t begin = uint64_t(base) + uint64_t(min_off);
  uint64_t end = uint64_t(base) + uint64_t(max_off) + 16u;
  if (end > uint64_t(physical_size) || begin >= end) {
    return r;
  }
  r.begin = uint32_t(begin);
  r.end = uint32_t(end);
  r.valid = true;
  return r;
}

// The single-mip form: the origin-aware envelope at origin (0,0). Kept as a
// named entry point because the promoted route and its pinned conformance test
// call it, but it is NOT a second implementation.
inline TextureLoadSourceRange ComputeStrictRgba8SourceRange(
    uint32_t base, uint32_t blocks_w, uint32_t blocks_h,
    uint32_t pitch_blocks_aligned, uint32_t physical_size) {
  return ComputeStrictRgba8SourceRangeAtOrigin(base, 0, 0, blocks_w, blocks_h,
                                               pitch_blocks_aligned,
                                               physical_size);
}

// ---- Destination layout ----

// Rows are 256-byte aligned (buffer-to-texture transfer alignment); the
// shader writes round_up(blocks_w, 8) blocks per row (full 8-block thread
// strides), of which only blocks_w * 4 bytes are logical texture content.
// Beyond written_row_bytes and past the last row, bytes are guard territory
// the dispatch must never touch.
struct TextureLoadDestLayout {
  uint32_t logical_row_bytes = 0;  // blocks_w * 4 — texture content.
  uint32_t written_row_bytes = 0;  // round8(blocks_w) * 4 — shader-written.
  uint32_t host_pitch = 0;         // align256(written_row_bytes).
  uint32_t rows = 0;               // blocks_h (exact y guard).
  uint32_t tail_guard_bytes = 0;
  uint32_t total_size = 0;
};

inline TextureLoadDestLayout ComputeTextureLoadDestLayout(uint32_t blocks_w,
                                                          uint32_t blocks_h) {
  TextureLoadDestLayout l;
  l.logical_row_bytes = blocks_w * 4;
  l.written_row_bytes = ((blocks_w + 7u) & ~7u) * 4;
  l.host_pitch = (l.written_row_bytes + 255u) & ~255u;
  l.rows = blocks_h;
  l.tail_guard_bytes = 256;
  l.total_size = l.host_pitch * l.rows + l.tail_guard_bytes;
  return l;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_LOAD_ELIGIBILITY_H_
