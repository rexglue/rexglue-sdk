/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_EDRAM_AUTHORITY_H_
#define XENIA_GPU_METAL_METAL_EDRAM_AUTHORITY_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Parses the strict 0/1 environment contract used by Metal pipeline gates.
// Unset inherits the caller's default; exact 1 enables, while exact 0 and
// malformed values disable so unsupported configuration never turns on by
// accident.
inline bool ParseMetalEnvironmentFlag(const char* value,
                                      bool default_enabled) {
  if (!value) {
    return default_enabled;
  }
  return value[0] == '1' && value[1] == '\0';
}

// A user-facing Metal switch: the config option (cvar) decides, unless the
// matching XE_METAL_* environment variable is set, which then overrides it with
// the strict contract above (exact 1 on, exact 0 / malformed off). Keeps the
// developer rollbacks and test harnesses working unchanged.
inline bool MetalSwitchEnabled(bool config_value, const char* env_value) {
  return ParseMetalEnvironmentFlag(env_value, config_value);
}

// Returns the smallest top-aligned texture height containing every pixel whose
// physical tile may be below end_tiles. Range shaders retain their exact
// start/end guards; this only avoids dispatching rows that are guaranteed to be
// beyond the operation. The result is clamped to the allocated texture.
// `scale_y` is the vertical draw resolution scale: texture rows are host rows.
inline uint32_t ComputeCanonicalRangeTextureHeight(
    uint32_t base_tiles, uint32_t pitch_tiles, uint32_t msaa_y_shift,
    uint32_t end_tiles, uint32_t texture_height, uint32_t scale_y = 1) {
  if (!pitch_tiles || end_tiles <= base_tiles || !texture_height) {
    return 0u;
  }
  uint64_t local_end_tiles = uint64_t(end_tiles) - base_tiles;
  uint64_t tile_rows =
      (local_end_tiles + pitch_tiles - 1u) / pitch_tiles;
  uint64_t pixel_rows =
      ((uint64_t(tile_rows) * xenos::kEDRAMTileHeightSamples) >> msaa_y_shift) *
      scale_y;
  return uint32_t(std::min<uint64_t>(pixel_rows, texture_height));
}

// Returns the top-aligned texture height every pixel of which lies in a
// physical tile row strictly above the row holding start_tiles: rows a range
// operation can skip from the top. Pairs with
// ComputeCanonicalRangeTextureHeight to bound a range operation to the rows
// [start, end) can touch. The range shaders keep their exact start/end guards.
inline uint32_t ComputeCanonicalRangeTextureRowStart(
    uint32_t base_tiles, uint32_t pitch_tiles, uint32_t msaa_y_shift,
    uint32_t start_tiles, uint32_t texture_height, uint32_t scale_y = 1) {
  if (!pitch_tiles || start_tiles <= base_tiles || !texture_height) {
    return 0u;
  }
  uint64_t tile_rows = (uint64_t(start_tiles) - base_tiles) / pitch_tiles;
  uint64_t pixel_rows =
      ((tile_rows * xenos::kEDRAMTileHeightSamples) >> msaa_y_shift) * scale_y;
  return uint32_t(std::min<uint64_t>(pixel_rows, texture_height));
}

// Chooses the height for a real-MSAA working view that resolves into the
// persistent single-sample render target. Metal requires the multisample and
// resolve attachments to have identical dimensions, so any non-empty use must
// allocate the full companion height.
inline uint32_t ComputeMsaaCompanionAllocationHeight(
    uint32_t current_height, uint32_t requested_touched_height,
    uint32_t max_height, uint32_t tile_row_height) {
  (void)current_height;
  (void)tile_row_height;
  if (!requested_touched_height || !max_height) {
    return 0u;
  }
  return max_height;
}

// Submission-ordered authority for the physical EDRAM tile space. This class
// deliberately contains no Metal objects: it decides which exact overlapping
// intervals need attachment->canonical packing or canonical->attachment
// materialization; MetalRenderTargetCache encodes those operations in order.
//
// Only one clean attachment cache is remembered per tile. Forgetting an older
// clean cache may cause an extra materialize, but can never return stale data.
class MetalEdramAuthorityTracker {
 public:
  using AttachmentId = uint64_t;

  enum class Location : uint8_t {
    kUnknown,
    kCanonical,
    kAttachment,
  };

  enum class SyncKind : uint8_t {
    kPackToCanonical,
    kMaterializeFromCanonical,
  };

  struct TileState {
    Location location = Location::kUnknown;
    AttachmentId authority_attachment = 0;
    AttachmentId clean_attachment = 0;
    uint64_t submission_order = 0;
  };

  struct SyncOperation {
    SyncKind kind;
    AttachmentId attachment;
    uint32_t start_tiles;
    uint32_t end_tiles;
    uint64_t producer_order;
    uint64_t consumer_order;
  };

  void ResetUnknown() {
    tiles_.fill(TileState{});
    ++version_;
  }

  // Bumped by every change to tile state EXCEPT an attachment re-stamping the
  // submission order of tiles it already holds as authority and clean cache
  // (every ordinary draw into an unchanged render target). With order stamps
  // drawn from one strictly increasing counter, SynchronizeAttachment(A, r)
  // is a no-op -- no operations, no state change -- whenever the version
  // still equals its value right after the previous SynchronizeAttachment(A,
  // r): that sync left every tile of r either A's authority or canonical and
  // clean for A, and a re-stamp by A keeps it so.
  uint64_t version() const { return version_; }

  // Drops every cached reference to a replaced or retired working view. The
  // caller should synchronize canonical first; if an attachment is still the
  // authority, conservatively make those tiles unknown rather than retain a
  // pointer to content that no longer exists.
  void InvalidateAttachment(AttachmentId attachment) {
    if (!attachment) {
      return;
    }
    bool changed = false;
    for (TileState& state : tiles_) {
      if (state.location == Location::kAttachment &&
          state.authority_attachment == attachment) {
        state.location = Location::kUnknown;
        state.authority_attachment = 0;
        state.clean_attachment = 0;
        changed = true;
      } else if (state.clean_attachment == attachment) {
        state.clean_attachment = 0;
        changed = true;
      }
    }
    if (changed) {
      ++version_;
    }
  }

  void MarkCanonical(uint32_t start_tiles, uint32_t end_tiles,
                     uint64_t submission_order) {
    ClampRange(start_tiles, end_tiles);
    bool changed = false;
    for (uint32_t tile = start_tiles; tile < end_tiles; ++tile) {
      TileState& state = tiles_[tile];
      if (submission_order < state.submission_order) {
        continue;
      }
      state.location = Location::kCanonical;
      state.authority_attachment = 0;
      // A canonical write invalidates every previously materialized view.
      state.clean_attachment = 0;
      state.submission_order = submission_order;
      changed = true;
    }
    if (changed) {
      ++version_;
    }
  }

  void MarkAttachment(AttachmentId attachment, uint32_t start_tiles,
                      uint32_t end_tiles, uint64_t submission_order) {
    if (!attachment) {
      return;
    }
    ClampRange(start_tiles, end_tiles);
    bool changed = false;
    for (uint32_t tile = start_tiles; tile < end_tiles; ++tile) {
      TileState& state = tiles_[tile];
      if (submission_order < state.submission_order) {
        continue;
      }
      if (state.location != Location::kAttachment ||
          state.authority_attachment != attachment ||
          state.clean_attachment != attachment) {
        changed = true;
      }
      state.location = Location::kAttachment;
      state.authority_attachment = attachment;
      state.clean_attachment = attachment;
      state.submission_order = submission_order;
    }
    if (changed) {
      ++version_;
    }
  }

  // Makes canonical current for the requested interval. Only tiles whose
  // newest value resides in an attachment generate pack operations; unknown
  // and already-canonical tiles are untouched.
  std::vector<SyncOperation> SynchronizeCanonical(
      uint32_t start_tiles, uint32_t end_tiles, uint64_t consumer_order) {
    return SynchronizeCanonicalImpl(start_tiles, end_tiles, consumer_order,
                                    /*compatible_attachment=*/0);
  }

  // Makes `attachment` a clean working view for the requested interval. Dirty
  // values in any other attachment are packed first, then only canonical tiles
  // not already cached by this attachment are materialized. Returned operation
  // order is therefore directly encodable in one Metal command stream.
  std::vector<SyncOperation> SynchronizeAttachment(
      AttachmentId attachment, uint32_t start_tiles, uint32_t end_tiles,
      uint64_t consumer_order) {
    if (!attachment) {
      return {};
    }
    ClampRange(start_tiles, end_tiles);
    std::vector<SyncOperation> operations = SynchronizeCanonicalImpl(
        start_tiles, end_tiles, consumer_order, attachment);
    for (uint32_t tile = start_tiles; tile < end_tiles;) {
      TileState& state = tiles_[tile];
      if (state.location != Location::kCanonical ||
          state.clean_attachment == attachment ||
          state.submission_order > consumer_order) {
        ++tile;
        continue;
      }
      uint64_t producer_order = state.submission_order;
      uint32_t operation_end = tile + 1;
      while (operation_end < end_tiles) {
        const TileState& next = tiles_[operation_end];
        if (next.location != Location::kCanonical ||
            next.clean_attachment == attachment ||
            next.submission_order > consumer_order) {
          break;
        }
        producer_order =
            producer_order > next.submission_order ? producer_order
                                                   : next.submission_order;
        ++operation_end;
      }
      operations.push_back({SyncKind::kMaterializeFromCanonical, attachment,
                            tile, operation_end, producer_order,
                            consumer_order});
      for (uint32_t materialized = tile; materialized < operation_end;
           ++materialized) {
        TileState& materialized_state = tiles_[materialized];
        materialized_state.clean_attachment = attachment;
        materialized_state.submission_order = consumer_order;
      }
      tile = operation_end;
    }
    if (!operations.empty()) {
      ++version_;
    }
    return operations;
  }

  const TileState& tile(uint32_t index) const { return tiles_[index]; }

 private:
  std::vector<SyncOperation> SynchronizeCanonicalImpl(
      uint32_t start_tiles, uint32_t end_tiles, uint64_t consumer_order,
      AttachmentId compatible_attachment) {
    ClampRange(start_tiles, end_tiles);
    std::vector<SyncOperation> operations;
    for (uint32_t tile = start_tiles; tile < end_tiles;) {
      TileState& state = tiles_[tile];
      if (state.location != Location::kAttachment ||
          state.authority_attachment == compatible_attachment ||
          state.submission_order > consumer_order) {
        ++tile;
        continue;
      }
      const AttachmentId source = state.authority_attachment;
      uint64_t producer_order = state.submission_order;
      uint32_t operation_end = tile + 1;
      while (operation_end < end_tiles) {
        const TileState& next = tiles_[operation_end];
        if (next.location != Location::kAttachment ||
            next.authority_attachment != source ||
            next.submission_order > consumer_order) {
          break;
        }
        producer_order =
            producer_order > next.submission_order ? producer_order
                                                   : next.submission_order;
        ++operation_end;
      }
      operations.push_back({SyncKind::kPackToCanonical, source, tile,
                            operation_end, producer_order, consumer_order});
      for (uint32_t packed = tile; packed < operation_end; ++packed) {
        TileState& packed_state = tiles_[packed];
        packed_state.location = Location::kCanonical;
        packed_state.authority_attachment = 0;
        packed_state.clean_attachment = source;
        packed_state.submission_order = consumer_order;
      }
      tile = operation_end;
    }
    if (!operations.empty()) {
      ++version_;
    }
    return operations;
  }
  static void ClampRange(uint32_t& start_tiles, uint32_t& end_tiles) {
    if (start_tiles > xenos::kEDRAMTileCount) {
      start_tiles = xenos::kEDRAMTileCount;
    }
    if (end_tiles > xenos::kEDRAMTileCount) {
      end_tiles = xenos::kEDRAMTileCount;
    }
    if (end_tiles < start_tiles) {
      end_tiles = start_tiles;
    }
  }

  std::array<TileState, xenos::kEDRAMTileCount> tiles_{};
  uint64_t version_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_EDRAM_AUTHORITY_H_
