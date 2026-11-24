/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_FAULT_EPISODE_H_
#define XENIA_GPU_METAL_METAL_FAULT_EPISODE_H_

// GPU fault EPISODE accounting, as a pure state machine so the grouping rules
// are pinned by the L0 suite instead of being validated only when a stochastic
// hardware fault happens to occur.
//
// WHY EPISODES EXIST: one GPU failure makes every in-flight submission complete
// with an error, and a revoked queue fails everything until it is recreated. So
// a session can log dozens of completion errors and many FAULTED encoder lines
// from a handful of real events. Measured on Viva Pinata (both_r1): 45
// completion-error headers across 19 swaps carried 5 FAULTED encoder lines and
// 2 queue revokes -- most plausibly ~2 episodes with a large collateral radius.
//
// Fault magnitude therefore needs FOUR counts, none of which substitutes for
// another: EPISODES, distinct failed submission serials, fault-bearing swaps,
// and FAULTED encoder lines.
//
// MEMBERSHIP RULE: an episode is keyed by the QUEUE EPOCH THE SUBMISSION WAS
// SUBMITTED UNDER -- never the epoch current when its callback is drained.
// Callbacks can arrive late, after the queue has already been revoked and
// recreated; keying on drain-time epoch would split one episode across two.
// Each submission carries its own epoch and episodes are tracked per-epoch
// concurrently.
// The ROOT is the LOWEST failed submission serial in the episode, NOT the
// first-reported one: completion callbacks may arrive out of submission order,
// so a lower serial reporting later is the truer root and revises it downward.

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

struct FaultEpisodeSummary {
  uint64_t episode = 0;
  uint64_t epoch = 0;
  uint64_t root_serial = 0;      // Lowest failed serial: the one to root-cause.
  uint64_t failed_submissions = 0;  // Members, i.e. blast radius.
  bool valid = false;
};

// Whether a failure could be admitted. Capacity exhaustion is NEVER resolved by
// eviction: an evicted epoch may still receive a late callback, and losing that
// attribution silently is the exact failure this accounting exists to prevent.
enum class EpisodeAdmission { kAdmitted, kNoCapacity };

class FaultEpisodeTracker {
 public:
  static constexpr uint32_t kMaxTrackedEpochs = 8;

  // Admits a failed `submission` issued under `submit_epoch`. Returns the
  // episode id, or 0 with kNoCapacity when every slot is still live -- the
  // caller must then fail loudly / disable further capture, never evict.
  uint64_t NoteMember(uint64_t submission, uint64_t submit_epoch,
                      EpisodeAdmission* admission) {
    EpochRecord* record = Find(submit_epoch);
    if (!record) {
      record = Acquire(submit_epoch);
      if (!record) {
        if (admission) {
          *admission = EpisodeAdmission::kNoCapacity;
        }
        return 0;
      }
    }
    if (admission) {
      *admission = EpisodeAdmission::kAdmitted;
    }
    if (!record->episode_id) {
      record->episode_id = ++counter_;
      record->root_serial = submission;
      record->members = 0;
    }
    // Out-of-order completion: a lower serial is the truer root. Serial 0 is
    // the unknown/unowned sentinel elsewhere and must never win the min().
    if (submission &&
        (!record->root_serial || submission < record->root_serial)) {
      record->root_serial = submission;
    }
    ++record->members;
    return record->episode_id;
  }

  // The queue advanced past this epoch (revoke/recreate) or we are tearing
  // down. Marks it closed -- it does NOT finalize, because a late callback can
  // still arrive and revise the root downward.
  void MarkEpochClosed(uint64_t epoch) {
    if (EpochRecord* record = Find(epoch)) {
      record->closed = true;
    }
  }

  // Finalizes ONLY when the epoch is closed AND no callbacks remain
  // outstanding for it. Until both hold, the root and member count are still
  // provisional. Frees the slot on success.
  FaultEpisodeSummary TryFinalize(uint64_t epoch, uint64_t outstanding) {
    EpochRecord* record = Find(epoch);
    if (!record || !record->closed || outstanding) {
      return FaultEpisodeSummary();
    }
    FaultEpisodeSummary summary;
    if (record->episode_id) {
      summary.episode = record->episode_id;
      summary.epoch = record->epoch;
      summary.root_serial = record->root_serial;
      summary.failed_submissions = record->members;
      summary.valid = true;
    }
    record->used = false;  // No faults => slot released with no summary.
    return summary;
  }

  // Epochs currently tracked, for the caller's finalize sweep.
  uint32_t tracked_epoch_count() const { return count_; }
  uint64_t tracked_epoch_at(uint32_t index) const {
    return records_[index].used ? records_[index].epoch : 0;
  }

  void BumpEpoch() { ++epoch_; }
  uint64_t epoch() const { return epoch_; }
  uint64_t episode_count() const { return counter_; }
  // Slots holding attribution that has not yet been finalized.
  uint32_t live_epochs() const {
    uint32_t live = 0;
    for (uint32_t i = 0; i < kMaxTrackedEpochs; ++i) {
      live += records_[i].used ? 1u : 0u;
    }
    return live;
  }

 private:
  struct EpochRecord {
    bool used = false;
    bool closed = false;
    uint64_t epoch = 0;
    uint64_t episode_id = 0;  // 0 until a fault occurs in this epoch.
    uint64_t root_serial = 0;
    uint64_t members = 0;
  };
  EpochRecord* Find(uint64_t epoch) {
    for (uint32_t i = 0; i < kMaxTrackedEpochs; ++i) {
      if (records_[i].used && records_[i].epoch == epoch) {
        return &records_[i];
      }
    }
    return nullptr;
  }
  EpochRecord* Acquire(uint64_t epoch) {
    for (uint32_t i = 0; i < kMaxTrackedEpochs; ++i) {
      if (!records_[i].used) {
        records_[i] = EpochRecord();
        records_[i].used = true;
        records_[i].epoch = epoch;
        if (i >= count_) {
          count_ = i + 1;
        }
        return &records_[i];
      }
    }
    return nullptr;  // Full: caller fails loudly. NEVER evict.
  }

  EpochRecord records_[kMaxTrackedEpochs];
  uint32_t count_ = 0;
  uint64_t epoch_ = 0;
  uint64_t counter_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_FAULT_EPISODE_H_
