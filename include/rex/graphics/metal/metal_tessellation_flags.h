/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TESSELLATION_FLAGS_H_
#define XENIA_GPU_METAL_METAL_TESSELLATION_FLAGS_H_

// port-xenios-tessellation task 4.6: the XE_METAL_TESSELLATION gate parse and
// the pure draw-route admissibility predicates, as PURE functions of their
// inputs so the L0 suite pins the decision matrix without a device or
// environment (the exact conventions of metal_depth_semantics_flags.h).
//
// Gate contract (promoted default-ON by implement-metal-host-tessellation,
// 2026-09-20): unset, empty and exact "1" are ON; exact "0" is OFF (the exact
// rollback, byte-identical to the pre-route OFF arm); anything else is
// kMalformed so the caller can warn once, and behaviorally means the default
// (ON).

#include <cstring>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

enum class MetalTessellationGate { kOff, kOn, kMalformed };

// Pure parse of the XE_METAL_TESSELLATION value (nullptr = unset = ON).
inline MetalTessellationGate ParseMetalTessellationGate(const char* value) {
  if (!value || !value[0] || !std::strcmp(value, "1")) {
    return MetalTessellationGate::kOn;
  }
  if (!std::strcmp(value, "0")) {
    return MetalTessellationGate::kOff;
  }
  return MetalTessellationGate::kMalformed;
}

// Malformed values take the default (ON) after the caller's warn-once.
inline bool MetalTessellationGateEngaged(MetalTessellationGate gate) {
  return gate != MetalTessellationGate::kOff;
}

// Whether the tessellation route's CPU-swap arena staging (SwapIndicesInto)
// can reproduce the guest index/factor stream exactly (the route CPU-swaps the stream into the upload arena and
// reads it on the host; vertex_index_endian stays kNone).
//
// 16-bit streams: the shared ::rex::gpu::PrimitiveProcessor already normalizes 32-bit
// swap modes on 16-bit indices down to kNone/k8in16, both of which
// SwapIndicesInto implements exactly.
// 32-bit streams (including the adaptive float factor stream): the staging
// helper's 32-bit branch implements only the full byte reversal, which is
// correct for kNone and k8in32 (the shapes seen from real guests) but NOT for
// k8in16/k16in32 on 32-bit words — those would silently corrupt factors, so
// the route fail-closed skips them instead (caught here, pinned by the L0
// endian oracle).
inline bool IsMetalTessellationIndexStreamSupported(xenos::IndexFormat format,
                                                    xenos::Endian endian) {
  if (format == xenos::IndexFormat::kInt16) {
    return true;
  }
  return endian == xenos::Endian::kNone || endian == xenos::Endian::k8in32;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_TESSELLATION_FLAGS_H_
