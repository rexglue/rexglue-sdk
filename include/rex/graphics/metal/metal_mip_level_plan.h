/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_MIP_LEVEL_PLAN_H_
#define XENIA_GPU_METAL_METAL_MIP_LEVEL_PLAN_H_

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rex/math.h"
#include "rex/graphics/metal/metal_texture_load_eligibility.h"
#include "rex/graphics/texture_util.h"
#include <rex/xenos/state.h>

// Pure (device-free) per-level plan for the OpenSpec 6.1 mipped sibling of the
// promoted strict-RGBA8 direct-load route. Extracted as a free function so it
// is the SINGLE planning authority: production calls it, and conformance tests
// it directly rather than through a device harness.
//
// Every per-level (offset, pitch, size, packed sub-tile) value is derived from
// the shared layout helpers (GetGuestTextureLayout + GetPackedMipOffset) -- the
// same selection D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl
// makes. No tiled, packing or endian math is re-implemented here.

namespace rex {
namespace graphics {
namespace metal {

struct MipLevelPlan {
  uint32_t host_level = 0;
  uint32_t slice = 0;           // array slice (stacked textures), else 0
  uint32_t depth = 1;           // 3D textures: the level's depth
  uint32_t z_stride_block_rows = 0;  // 3D: guest z slice stride (block rows)
  uint32_t guest_offset = 0;    // absolute guest byte address of the level
  uint32_t pitch_blocks = 0;    // row pitch in blocks (== texels at 32bpp)
  uint32_t width = 0;           // level texels
  uint32_t height = 0;
  uint32_t px = 0;              // packed sub-tile block origin
  uint32_t py = 0;
  uint32_t pz = 0;              // 3D packed tail: depth origin (slices)
  // CONSERVATIVE ENVELOPE, absolute: min/max over the tiled source offsets,
  // widened to the last group's end. It is what RequestConsumedRange and the
  // planner need -- a surplus consumed range can only cause an extra hazard
  // flush, never a missed one -- and it is NOT the exact set of bytes the load
  // reads. A tiled walk leaves holes inside these bounds; anything needing an
  // exact set must preserve the per-group union rather than reuse these fields.
  uint32_t consumed_begin = 0;  // inclusive
  uint32_t consumed_end = 0;    // exclusive
};

// Memo for the exact-read-range scan, which is the planner's only non-trivial
// cost: ComputeStrictRgba8SourceRangeAtOrigin evaluates Tiled2D once per
// 4-block group over the padded grid, measured at ~0.53 ms for a 1280x720
// level on an M4. The result depends ONLY on (px, py, blocks_w, blocks_h,
// pitch_blocks) -- never on the guest address -- so it is identical for every
// texture of a shape and the promoted single-mip route re-derives the same
// answer on every miss without this.
//
// It is a pure memo, not a second implementation: a miss calls the one
// authority and stores what it returned, so a memoized plan is equal to an
// unmemoized plan by construction (and pinned by conformance). Passing nullptr
// is always valid and always gives the same plan.
class MipLevelRangeMemo {
 public:
  TextureLoadSourceRange Get(uint32_t px, uint32_t py, uint32_t blocks_w,
                             uint32_t blocks_h, uint32_t pitch_blocks) {
    Key key{px, py, blocks_w, blocks_h, pitch_blocks};
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      return it->second;
    }
    // Relative to offset 0 and unbounded: the caller applies the texture's own
    // guest address and the real buffer bound, which are NOT part of the key.
    TextureLoadSourceRange range = ComputeStrictRgba8SourceRangeAtOrigin(
        0, px, py, blocks_w, blocks_h, pitch_blocks, UINT32_MAX);
    entries_.emplace(key, range);
    return range;
  }

  // Derived scan results keyed by shape; cheap to recompute and meaningless
  // once the textures are gone.
  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

 private:
  struct Key {
    uint32_t px, py, w, h, pitch;
    bool operator==(const Key& o) const {
      return px == o.px && py == o.py && w == o.w && h == o.h &&
             pitch == o.pitch;
    }
  };
  struct KeyHasher {
    size_t operator()(const Key& k) const {
      size_t hash = size_t(k.w) * 0x9E3779B1u;
      hash ^= size_t(k.h) << 16;
      hash ^= size_t(k.pitch) * 0x85EBCA6Bu;
      hash ^= (size_t(k.px) << 8) ^ (size_t(k.py) << 24);
      return hash;
    }
  };
  std::unordered_map<Key, TextureLoadSourceRange, KeyHasher> entries_;
};

// Inputs the planner needs, all already available on the shared TextureKey and
// its layout. Keeping them explicit is what makes the planner device-free.
struct MipLevelPlanInputs {
  uint32_t base_address = 0;  // key.base_page << 12
  uint32_t mip_address = 0;   // key.mip_page << 12
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_max_level = 0;  // from the KEY
  uint32_t array_size = 1;     // stacked 2D: slices, each at the level's stride
  // 3D (volume) texture: array_size is the base depth; one plan entry per
  // level covering the whole volume (the load shader's 3D path), consumed
  // range = the level's whole guest extent.
  bool is_3d = false;
  // Guest block geometry for the envelope scan (see BuildMipLevelPlan): bytes
  // per guest block and the block's edge in texels (4 for DXT/CTX1, else 1).
  uint32_t bytes_per_block = 4;
  uint32_t block_dim = 1;
  // Guest block WIDTH when it differs from block_dim (2 for the 4:2:2 texel
  // pairs); 0 means block_dim.
  uint32_t block_width = 0;
  // 1D texture: one row per level; a zero fetch pitch (legal for 1D, where
  // the row pitch addresses nothing) takes the width-derived pitch.
  bool is_1d = false;
  uint32_t guest_buffer_size = 0;
  bool load_base = false;
  bool load_mips = false;
};

// Builds the COMPLETE plan for the requested regions. Returns false -- meaning
// the caller must fall back to the legacy cache atomically, before encoding
// anything -- if any level cannot be planned or validated.
//
// `layout` must be the texture's own guest layout, and `in.mip_max_level` the
// KEY's level count; the two are cross-checked so a descriptor built from one
// can never disagree with a plan built from the other.
//
// `memo` is an optional pure cache of the per-shape read-range scan (see
// MipLevelRangeMemo). It changes cost only, never the plan.
inline bool BuildMipLevelPlan(const MipLevelPlanInputs& in,
                              const ::rex::xenos::GuestTextureLayout& layout,
                              std::vector<MipLevelPlan>* out,
                              MipLevelRangeMemo* memo = nullptr) {
  if (!out || (!in.load_base && !in.load_mips)) {
    return false;
  }
  out->clear();
  // Key/layout agreement: the layout clamps max_level to what the dimensions
  // can actually hold, so a key claiming more levels than the layout stores is
  // a planning error rather than something to silently truncate.
  if (in.mip_max_level != layout.max_level) {
    return false;
  }
  uint32_t packed = layout.packed_level;

  // The envelope scan walks 4-byte groups of a 32bpp tiled layout. Other
  // block sizes are expressed in that unit: a guest block of bytes_per_block
  // bytes is bytes_per_block / 4 groups wide (or one group covers 4 /
  // bytes_per_block texels), and block-compressed formats have block_dim x
  // block_dim texels per block. Before 2026-10-16 the texel width and height
  // were passed as groups, which put a 3968x4080 DXT5 level's envelope 64 MB
  // past its base (past the end of guest memory: "plan-failed", Rage) and
  // made every 8/16bpp consumed range 2-4x too wide.
  const uint32_t bpb = std::max(in.bytes_per_block, 1u);
  const uint32_t block_dim = std::max(in.block_dim, 1u);
  const uint32_t block_w = in.block_width ? in.block_width : block_dim;
  auto to_scan_w = [&](uint32_t texels) -> uint32_t {
    uint32_t blocks = (texels + block_w - 1u) / block_w;
    return bpb >= 4u ? blocks * (bpb / 4u)
                     : std::max((blocks * bpb + 3u) / 4u, 1u);
  };
  auto to_scan_h = [&](uint32_t texels) -> uint32_t {
    return std::max((texels + block_dim - 1u) / block_dim, 1u);
  };

  auto plan_one = [&](uint32_t level, uint32_t slice, uint64_t guest_offset,
                      uint32_t pitch_bytes, bool is_packed) -> bool {
    MipLevelPlan p;
    p.host_level = level;
    p.slice = slice;
    p.pitch_blocks = pitch_bytes / 4u;  // 32bpp: block == texel == 4 bytes
    p.width = std::max(in.width >> level, 1u);
    p.height = std::max(in.height >> level, 1u);
    if (in.is_1d && !p.pitch_blocks) {
      // One row: the pitch addresses nothing; use the level's own row size
      // (in 4-byte groups, at least one) so the scan and loader have a width.
      uint32_t row_bytes =
          ((p.width + block_w - 1u) / block_w) * bpb;
      p.pitch_blocks = std::max((row_bytes + 3u) / 4u, 1u);
    }
    if (is_packed) {
      uint32_t px = 0, py = 0, pz = 0;
      ::rex::xenos::GetPackedMipOffset(in.width, in.height, 1,
                                       xenos::TextureFormat::k_8_8_8_8, level,
                                       px, py, pz);
      p.px = px;
      p.py = py;
    }
    if (!p.pitch_blocks || !p.width || !p.height) {
      return false;
    }
    // 64-bit throughout, validated, THEN narrowed: a guest pointer plus a mip
    // offset can exceed 32 bits before any bounds check would see it.
    if (guest_offset >= in.guest_buffer_size) {
      return false;
    }
    p.guest_offset = uint32_t(guest_offset);
    const uint32_t scan_px = to_scan_w(p.px);
    const uint32_t scan_py = to_scan_h(p.py);
    const uint32_t scan_w = to_scan_w(p.width);
    const uint32_t scan_h = to_scan_h(p.height);
    TextureLoadSourceRange r =
        memo ? memo->Get(scan_px, scan_py, scan_w, scan_h, p.pitch_blocks)
             : ComputeStrictRgba8SourceRangeAtOrigin(0, scan_px, scan_py,
                                                     scan_w, scan_h,
                                                     p.pitch_blocks,
                                                     UINT32_MAX);
    if (!r.valid) {
      return false;
    }
    uint64_t begin = guest_offset + uint64_t(r.begin);
    uint64_t end = guest_offset + uint64_t(r.end);
    if (end > uint64_t(in.guest_buffer_size) || begin >= end) {
      return false;
    }
    p.consumed_begin = uint32_t(begin);
    p.consumed_end = uint32_t(end);
    out->push_back(p);
    return true;
  };

  if (in.is_3d) {
    // Volume levels: whole-level entries, no per-slice scan.
    // Packed levels (2026-10-26): every level from the tail's first is stored
    // in the tail's layout entry and placed by its (x, y, z) origin there,
    // the same rule the 2D path and the D3D12 loader use. Until now the plan
    // refused packed 3D tails and the legacy cache drops 3D mips, so every
    // mipped volume served its base level only.
    auto plan_3d = [&](uint32_t level, uint64_t guest_offset,
                       const ::rex::xenos::GuestTextureLayout::Level& lv,
                       bool is_packed) {
      MipLevelPlan p;
      p.host_level = level;
      p.slice = 0;
      p.pitch_blocks = lv.row_pitch_bytes / 4u;
      p.width = std::max(in.width >> level, 1u);
      p.height = std::max(in.height >> level, 1u);
      p.depth = std::max(in.array_size >> level, 1u);
      p.z_stride_block_rows = lv.z_layer_stride_block_rows;
      if (is_packed) {
        uint32_t px = 0, py = 0, pz = 0;
        ::rex::xenos::GetPackedMipOffset(in.width, in.height, in.array_size,
                                         xenos::TextureFormat::k_8_8_8_8,
                                         level, px, py, pz);
        p.px = px;
        p.py = py;
        p.pz = pz;
      }
      if (!p.pitch_blocks || !p.width || !p.height ||
          guest_offset >= in.guest_buffer_size) {
        return false;
      }
      uint64_t end = guest_offset + uint64_t(lv.level_data_extent_bytes);
      if (end > uint64_t(in.guest_buffer_size) || end <= guest_offset) {
        return false;
      }
      p.guest_offset = uint32_t(guest_offset);
      p.consumed_begin = p.guest_offset;
      p.consumed_end = uint32_t(end);
      out->push_back(p);
      return true;
    };
    if (in.load_base) {
      if (!in.base_address ||
          !plan_3d(0, in.base_address, layout.base, packed == 0)) {
        return false;
      }
    }
    if (in.load_mips) {
      if (!in.mip_max_level || !in.mip_address) {
        return false;
      }
      for (uint32_t level = 1; level <= in.mip_max_level; ++level) {
        uint32_t store =
            packed == UINT32_MAX ? level : std::min(level, packed);
        if (store >= rex::countof(layout.mips) ||
            !plan_3d(level,
                     uint64_t(in.mip_address) +
                         uint64_t(layout.mip_offsets_bytes[store]),
                     layout.mips[store],
                     packed != UINT32_MAX && level >= packed)) {
          return false;
        }
      }
    }
    return !out->empty();
  }
  if (in.load_base) {
    if (!in.base_address) {
      return false;
    }
    // packed_level == 0 means the BASE itself lives in a packed mip tail stored
    // at the base address; the shared loader keeps a SEPARATE mips tail at the
    // mip address. Level 0 is the tail's largest level, so its sub-tile origin
    // is whatever GetPackedMipOffset reports for level 0.
    for (uint32_t slice = 0; slice < in.array_size; ++slice) {
      if (!plan_one(0, slice,
                    uint64_t(in.base_address) +
                        uint64_t(slice) * layout.base.array_layer_stride_bytes,
                    layout.base.row_pitch_bytes, packed == 0)) {
        return false;
      }
    }
  }
  if (in.load_mips) {
    if (!in.mip_max_level || !in.mip_address) {
      return false;
    }
    for (uint32_t level = 1; level <= in.mip_max_level; ++level) {
      // The layout entry this level is STORED in: its own for a non-packed
      // level, the packed tail's entry for every packed level.
      uint32_t store =
          packed == UINT32_MAX ? level : std::min(level, packed);
      if (store >= rex::countof(layout.mips)) {
        return false;
      }
      bool is_packed = packed != UINT32_MAX && level >= packed;
      for (uint32_t slice = 0; slice < in.array_size; ++slice) {
        if (!plan_one(level, slice,
                      uint64_t(in.mip_address) +
                          uint64_t(layout.mip_offsets_bytes[store]) +
                          uint64_t(slice) *
                              layout.mips[store].array_layer_stride_bytes,
                      layout.mips[store].row_pitch_bytes, is_packed)) {
          return false;
        }
      }
    }
  }
  return !out->empty();
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_MIP_LEVEL_PLAN_H_
