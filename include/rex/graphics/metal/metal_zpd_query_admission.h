/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZPD_QUERY_ADMISSION_H_
#define XENIA_GPU_METAL_METAL_ZPD_QUERY_ADMISSION_H_

#include <cstdint>
#include <string>

#include "rex/graphics/metal/metal_settings.h"

namespace rex {
namespace graphics {
namespace metal {

// How occlusion queries are answered. Copied from the fork's command processor header, where the PM4 layer owned it.
enum class ZPDMode {
  kFake,     // Fake sample counts, no real GPU queries.
  kFast,     // Real queries with speculative cached writes.
  kFastAlt,  // Fast queries, but preserves cached zeroes.
  kStrict,   // Real queries, waits before writeback.
};

// The fork's pool size, which its query slot table is sized by.
constexpr uint32_t kZPDQueryPoolCapacity = 8192;

// Reads the occlusion-query mode from the setting the fork kept in a cvar; same four spellings, same default.
inline ZPDMode GetZPDMode() {
  const std::string& mode = ::rex::graphics::metal::GetMetalSettings().occlusion_query.Get();
  if (mode == "fake") {
    return ZPDMode::kFake;
  }
  if (mode == "strict") {
    return ZPDMode::kStrict;
  }
  if (mode == "fast-alt") {
    return ZPDMode::kFastAlt;
  }
  return ZPDMode::kFast;
}

// Pure value form of MetalCommandProcessor::OpenZPDQuery's decision: given the
// pool, encoder, and submission state, may a host query segment open now?
//
// Extracted as a device-free predicate because the interesting cases -- pool
// exhaustion under each ZPD mode -- are otherwise reachable only by driving a
// live command processor through 8192 concurrent segments. The production path
// builds these inputs and calls DecideZPDQueryAdmission, so the two share ONE
// decision and a rule deleted in production fails the conformance suite.
enum class ZPDQueryAdmission {
  // A slot is available; acquire it and begin counting.
  kAcquire,
  // Not now, retry at the next opportunity. No side effect.
  kDefer,
  // Strict only: the submission that would free a slot is the one still being
  // encoded, so the caller must commit it before it can ever complete.
  kDeferAfterClosingSubmission,
  // Strict only: block until the oldest pending resolve's submission
  // completes, drain, then re-decide.
  kWaitForOldestResolve,
  // No slot, and waiting is either impossible or not wanted. Fast modes take
  // this to keep the guest moving; the shared code substitutes a minimal
  // accumulated value rather than stalling.
  kPoolExhausted,
  // Unusable state; fall back without opening a segment.
  kFailed,
};

struct ZPDQueryAdmissionInputs {
  // The visibility pool exists and has a mapped buffer.
  bool pool_ready = false;
  // A segment is already counting. The shared code closes or discards before
  // reopening, so this means the caller lost track of one; opening a second
  // would strand the first slot's offset for the rest of the pass.
  bool query_already_open = false;
  // A render encoder is open AND was built with the visibility buffer
  // attached. Metal only accepts that buffer on the pass descriptor, so an
  // encoder created before the pool existed can never carry a query.
  bool encoder_can_carry_query = false;
  // A free slot is available, evaluated after any drain the caller performed.
  bool has_free_slot = false;
  // kFast or kFastAlt: favor forward progress over an exact count.
  bool fast_mode = false;
  // At least one closed segment is awaiting its submission's completion.
  bool has_pending_resolve = false;
  // Submission of the oldest pending resolve; meaningless if
  // has_pending_resolve is false.
  uint64_t oldest_pending_submission = 0;
  // The id the command buffer currently being encoded will receive.
  uint64_t current_encoding_submission = 0;
  // Whether this call site permits committing the open command buffer.
  bool can_close_submission = false;
};

constexpr ZPDQueryAdmission DecideZPDQueryAdmission(
    const ZPDQueryAdmissionInputs& in) {
  if (!in.pool_ready || in.query_already_open) {
    return ZPDQueryAdmission::kFailed;
  }
  if (!in.encoder_can_carry_query) {
    return ZPDQueryAdmission::kDefer;
  }
  if (in.has_free_slot) {
    return ZPDQueryAdmission::kAcquire;
  }
  // Exhausted from here down.
  if (in.fast_mode) {
    return ZPDQueryAdmission::kPoolExhausted;
  }
  if (!in.has_pending_resolve) {
    // Nothing is in flight, so no slot can ever come back by waiting.
    return ZPDQueryAdmission::kPoolExhausted;
  }
  if (in.oldest_pending_submission >= in.current_encoding_submission) {
    // The freeing work has not been committed yet. Waiting on it without
    // committing first would deadlock.
    return in.can_close_submission
               ? ZPDQueryAdmission::kDeferAfterClosingSubmission
               : ZPDQueryAdmission::kDefer;
  }
  return ZPDQueryAdmission::kWaitForOldestResolve;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZPD_QUERY_ADMISSION_H_
