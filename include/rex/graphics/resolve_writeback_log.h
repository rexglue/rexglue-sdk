/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_RESOLVE_WRITEBACK_LOG_H_
#define XENIA_GPU_RESOLVE_WRITEBACK_LOG_H_

#include <cstddef>
#include <cstdint>

namespace rex {
namespace graphics {

// Backend-neutral resolve-writeback telemetry. One record is emitted per
// resolve copy at the point where the backend has made the resolved data
// coherent in guest RAM (Metal: after the tiled/endian scatter; Vulkan: after
// the readback copy into guest RAM). Live temporal resources (e.g. the
// exposure luminance pyramid tips) are identified from these records by
// resolve semantics — format, dimensions, ordering, coherence — never by
// addresses captured in another session or backend.
// Gated by XE_GPU_RESOLVE_WB_LOG=1; when unset, no record is built and
// behavior is identical.
struct ResolveWritebackRecord {
  const char* backend;     // "metal" | "vulkan"
  uint64_t frame;          // backend frame/swap counter at coherence time
  uint64_t sequence;       // backend draw/copy ordering key within the run
  uint32_t guest_address;  // current guest physical dest base (this session)
  uint32_t width;          // resolve rectangle, dest texels
  uint32_t height;
  uint32_t format;   // RB_COPY_DEST_INFO.copy_dest_format (xenos::ColorFormat)
  uint32_t endian;   // RB_COPY_DEST_INFO.copy_dest_endian (Endian128)
  uint32_t swap;     // RB_COPY_DEST_INFO.copy_dest_swap (BGRA channel pack)
  uint32_t extent_bytes;  // coherent extent hashed below, relative to base
  // First post-pack 32-bit word of this backend's coherent extent, read back
  // from guest RAM (i.e. after Metal's tiled/endian scatter or Vulkan's
  // readback copy) — the value a guest CPU load at that location observes.
  uint32_t raw_word0;
  uint64_t guest_hash;       // XXH3 of the post-pack coherent extent
  const void* host_virtual;  // live virtual-mirror host mapping of the base
  // Age in frames of the data made coherent: 0 = produced by this frame's
  // resolve; Vulkan fast readback returns the previous buffered copy (1).
  int32_t readback_age_frames;
};

// True when XE_GPU_RESOLVE_WB_LOG is set (and not "0"); read once.
bool IsResolveWritebackLogEnabled();

// XXH3 fingerprint of the post-pack guest-RAM extent. `data` may be null
// (returns 0) so call sites don't need their own mapping checks.
uint64_t HashResolveWritebackExtent(const void* data, size_t size);

// Formats and emits one record. Call only when IsResolveWritebackLogEnabled().
void LogResolveWriteback(const ResolveWritebackRecord& record);

}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_RESOLVE_WRITEBACK_LOG_H_
