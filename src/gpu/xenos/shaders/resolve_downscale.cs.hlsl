/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Compute shader to downscale scaled resolve buffer data back to 1x resolution.
// Operates on 32x32 tiled data format used by Xbox 360.
// Each thread handles one output pixel (one 32x32 tile = 1024 threads).
//
// By default, picks the top-left pixel of each scale_x * scale_y block.
// When xe_downscale_half_pixel_offset is set, samples from (scale/2, scale/2)
// within each block to compensate for the half-pixel offset becoming a
// full-pixel offset at higher resolutions.

cbuffer XeResolveDownscaleConstants : register(b0) {
  uint xe_downscale_scale_x;         // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_scale_y;         // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_pixel_size_log2; // 0=8bit, 1=16bit, 2=32bit, 3=64bit
  uint xe_downscale_tile_count;      // Number of 32x32 tiles to process
  // When non-zero, apply half-pixel offset correction by sampling from
  // (scale/2, scale/2) within each scaled block instead of (0, 0).
  // This compensates for the D3D9-style half-pixel offset used by Xbox 360
  // games, which at Nx resolution scaling shifts rendered content by
  // (N/2, N/2) host pixels.
  uint xe_downscale_half_pixel_offset;
};

ByteAddressBuffer xe_resolve_source : register(t0);
RWByteAddressBuffer xe_resolve_dest : register(u0);

// Groupshared memory for coalescing sub-32-bit writes
// Max tile size at 1x is 32*32*8 bytes = 8KB for 64-bit pixels
// We only need this for 8-bit and 16-bit where we coalesce to 32-bit stores
groupshared uint gs_tile_data[32 * 32];

// Thread group processes one 32x32 tile worth of output
[numthreads(32, 32, 1)]
void main(uint3 xe_group_id : SV_GroupID,
          uint3 xe_thread_id_in_group : SV_GroupThreadID,
          uint xe_group_thread_index : SV_GroupIndex) {
  uint tile_index = xe_group_id.x;

  // Early out if beyond tile count
  [branch] if (tile_index >= xe_downscale_tile_count) {
    return;
  }

  uint row = xe_thread_id_in_group.y;
  uint column = xe_thread_id_in_group.x;
  uint pixel_index = row * 32 + column;  // 0-1023

  uint pixel_size = 1u << xe_downscale_pixel_size_log2;
  uint tile_size_1x = 32 * 32 * pixel_size;
  uint scale_xy = xe_downscale_scale_x * xe_downscale_scale_y;
  uint tile_size_scaled = tile_size_1x * scale_xy;

  // Compute offset within each scaled block to sample from.
  // Without half-pixel correction: sample from (0, 0) = linear offset 0.
  // With half-pixel correction: sample from (scale/2, scale/2) to compensate
  // for the D3D9-style half-pixel offset shifting content by (N/2, N/2) pixels
  // at Nx resolution.
  uint block_sample_offset = 0u;
  [branch] if (xe_downscale_half_pixel_offset != 0u && scale_xy > 1u) {
    uint offset_x = xe_downscale_scale_x >> 1u;
    uint offset_y = xe_downscale_scale_y >> 1u;
    block_sample_offset = offset_x + offset_y * xe_downscale_scale_x;
  }

  // Source offset: base of the scaled block plus offset within block
  uint src_offset = tile_index * tile_size_scaled +
                    pixel_index * pixel_size * scale_xy +
                    block_sample_offset * pixel_size;

  // Destination offset in 1x buffer
  uint dst_offset = tile_index * tile_size_1x +
                    pixel_index * pixel_size;

  // Copy pixel based on size
  [branch] switch (xe_downscale_pixel_size_log2) {
    case 0: {  // 8-bit - use groupshared to coalesce 4 bytes into 32-bit writes
      // Load the byte value
      uint byte_val = (xe_resolve_source.Load(src_offset & ~3u) >>
                       ((src_offset & 3u) * 8u)) & 0xFFu;

      // Each group of 4 threads packs into one uint
      uint pack_index = pixel_index >> 2;   // Which uint (0-255)
      uint byte_pos = pixel_index & 3u;     // Which byte in uint (0-3)

      // Pack byte into shared memory uint using atomics within group
      uint contribution = byte_val << (byte_pos * 8u);
      if (byte_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      GroupMemoryBarrierWithGroupSync();
      if (byte_pos != 0u) {
        InterlockedOr(gs_tile_data[pack_index], contribution);
      }
      GroupMemoryBarrierWithGroupSync();

      // First thread of each 4 writes the packed uint
      if (byte_pos == 0u) {
        xe_resolve_dest.Store(tile_index * (32 * 32) + pack_index * 4,
                              gs_tile_data[pack_index]);
      }
      break;
    }
    case 1: {  // 16-bit - use groupshared to coalesce 2 shorts into 32-bit writes
      // Load the short value
      uint short_val = (xe_resolve_source.Load(src_offset & ~3u) >>
                        ((src_offset & 2u) * 8u)) & 0xFFFFu;

      // Each group of 2 threads packs into one uint
      uint pack_index = pixel_index >> 1;   // Which uint (0-511)
      uint short_pos = pixel_index & 1u;    // Which short in uint (0-1)

      // Pack short into shared memory
      uint contribution = short_val << (short_pos * 16u);
      if (short_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      GroupMemoryBarrierWithGroupSync();
      if (short_pos != 0u) {
        InterlockedOr(gs_tile_data[pack_index], contribution);
      }
      GroupMemoryBarrierWithGroupSync();

      // First thread of each 2 writes the packed uint
      if (short_pos == 0u) {
        xe_resolve_dest.Store(tile_index * (32 * 32 * 2) + pack_index * 4,
                              gs_tile_data[pack_index]);
      }
      break;
    }
    case 2: {  // 32-bit - direct copy
      xe_resolve_dest.Store(dst_offset, xe_resolve_source.Load(src_offset));
      break;
    }
    case 3: {  // 64-bit - direct copy
      xe_resolve_dest.Store2(dst_offset, xe_resolve_source.Load2(src_offset));
      break;
    }
  }
}
