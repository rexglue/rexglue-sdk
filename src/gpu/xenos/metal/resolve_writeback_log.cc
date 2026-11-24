/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/graphics/resolve_writeback_log.h"

#include <cstdlib>

#include <rex/logging/logger.h>
#include <xxhash.h>

namespace rex {
namespace graphics {

bool IsResolveWritebackLogEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XE_GPU_RESOLVE_WB_LOG");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

uint64_t HashResolveWritebackExtent(const void* data, size_t size) {
  if (!data || !size) {
    return 0;
  }
  return XXH3_64bits(data, size);
}

void LogResolveWriteback(const ResolveWritebackRecord& record) {
  REXLOG_INFO(
      "ResolveWB backend={} frame={} seq={} addr={:08X} dim={}x{} fmt={} "
      "endian={} swap={} extent={} word0={:08X} hash={:016x} hostva={} age={}",
      record.backend, record.frame, record.sequence, record.guest_address,
      record.width, record.height, record.format, record.endian, record.swap,
      record.extent_bytes, record.raw_word0, record.guest_hash,
      record.host_virtual, record.readback_age_frames);
}

}  // namespace gpu
}  // namespace rex
