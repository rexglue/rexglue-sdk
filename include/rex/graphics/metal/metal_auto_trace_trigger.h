/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_AUTO_TRACE_TRIGGER_H_
#define XENIA_GPU_METAL_METAL_AUTO_TRACE_TRIGGER_H_

// Condition-triggered NEXT-FRAME trace capture policy.
//
// SCOPE: this is deliberately the contained half of "capture the bug when it
// happens": it arms the existing single-frame pending-trace machinery
// (CommandProcessor::RequestFrameTrace) when a condition fires at swap, so the
// FOLLOWING frame is captured self-contained. It cannot capture frames before
// the trigger -- a pre-trigger ring is a separate trace-architecture design
// (bounded initial-state snapshots per retained frame), not this type.
//
// WHY A POLICY TYPE: firing decisions must be debounced (cooldown) and
// budgeted (max captures per session) or an artifact that persists across
// frames would convert the session into disk-filling capture churn. The
// decision logic is pure and device-free so the L0 conformance target covers
// the contract directly: what fires, what outranks what, when the budget and
// cooldown suppress.
//
// CONDITIONS (evaluated once per swap, all observed since the previous swap):
//   - GPU fault: any Metal command buffer completed with
//     MTLCommandBufferStatusError. Highest priority; a faulting frame is
//     always worth a capture.
//   - Actionable draw skips: MetalDrawSkipAccounting::frame_actionable_count()
//     at or above a threshold. Expected pruning (kInfo severity) can never
//     arm a capture -- that separation is the whole point of reason-coded
//     skip accounting.
// A manual hotkey capture (GraphicsSystem::RequestFrameTrace) is unaffected:
// it shares the pending-trace machinery, and the caller must not auto-arm
// while a trace is already pending or streaming.

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

struct MetalAutoTraceConfig {
  bool enabled = false;
  // Fire when actionable skips this frame >= threshold. 0 disables the skip
  // condition entirely (fault-only arming).
  uint32_t skip_threshold = 1;
  // Fire on any GPU fault observed since the previous swap.
  bool on_gpu_fault = true;
  // Minimum frames between fires. The FIRST fire is never delayed.
  uint32_t cooldown_frames = 300;
  // Session budget: total automatic captures. 0 means never fire.
  uint32_t max_captures = 3;
};

class MetalAutoTraceTrigger {
 public:
  struct Decision {
    bool fire = false;
    // Stable reason token for the log line ("gpu-fault" / "skip-threshold").
    const char* reason = nullptr;
  };

  explicit MetalAutoTraceTrigger(const MetalAutoTraceConfig& config)
      : config_(config) {}

  const MetalAutoTraceConfig& config() const { return config_; }
  uint32_t fired_count() const { return fired_count_; }

  // Evaluate once per swap boundary — successful, suppressed, or headless.
  // frame_index must be strictly monotonic across calls; the caller supplies
  // a dedicated finalization counter (NOT the presented-swap counter, which
  // stalls on suppressed frames and would freeze the cooldown window).
  Decision Evaluate(uint64_t frame_index, uint64_t actionable_skips,
                    uint32_t gpu_faults) {
    Decision decision;
    if (!config_.enabled || fired_count_ >= config_.max_captures) {
      return decision;
    }
    if (fired_count_ > 0 &&
        frame_index - last_fire_frame_ < config_.cooldown_frames) {
      return decision;
    }
    if (config_.on_gpu_fault && gpu_faults > 0) {
      decision.fire = true;
      decision.reason = "gpu-fault";
    } else if (config_.skip_threshold > 0 &&
               actionable_skips >= config_.skip_threshold) {
      decision.fire = true;
      decision.reason = "skip-threshold";
    }
    if (decision.fire) {
      ++fired_count_;
      last_fire_frame_ = frame_index;
    }
    return decision;
  }

 private:
  MetalAutoTraceConfig config_;
  uint32_t fired_count_ = 0;
  uint64_t last_fire_frame_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_AUTO_TRACE_TRIGGER_H_
