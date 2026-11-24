/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_CACHE_CLEAR_ADMISSION_H_
#define XENIA_GPU_METAL_METAL_CACHE_CLEAR_ADMISSION_H_

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

// Pure value form of "may a latched cache-clear request release resources
// now?", following the deferred ordering contract:
//
//   1. finish encoder
//   2. wait for all submitted work
//   3. pump completion actions
//   4. publish/drain memexport and render-target writebacks
//   5. clear derived cache state
//
// Steps 1-4 are preconditions for step 5 and this predicate decides which one
// is still outstanding. Releasing a resource an unfinished submission may read
// is the whole hazard, and dropping a queued writeback would discard
// guest-visible data that has not yet reached RAM -- guest RAM is the
// steady-state authority for it, so a pending producer is authoritative state,
// not derived cache content.
//
// Extracted as a device-free predicate for the same reason the ZPD admission
// decision was (metal_zpd_query_admission.h): the dangerous states -- a
// request arriving mid-encoder, arriving with submissions in flight, arriving
// with a writeback still unpublished -- are awkward to reach on a live command
// processor and trivial to enumerate as values. Production builds these inputs
// and calls this function, so a rule deleted in production fails conformance.
enum class CacheClearAdmission {
  // Nothing latched. The no-request hot path: the caller must do no work,
  // add no commit, and take no wait.
  kNoRequest,
  // An encoder is open or submitted work has not completed. The caller may
  // close the submission here, so end the encoder and await completion.
  kEndSubmissionFirst,
  // The device is idle but GPU-produced guest data is still queued. Publish it
  // to guest RAM before anything is released.
  kPublishPendingFirst,
  // Cannot progress at this call site. The request stays latched and is
  // re-decided at the next boundary. Never a drop.
  kDefer,
  // Every precondition holds: release derived cache state.
  kExecute,
};

struct CacheClearAdmissionInputs {
  // A ClearCaches() request has been latched and not yet executed.
  bool request_latched = false;
  // A render encoder is currently open.
  bool encoder_open = false;
  // Encoded work exists in a command buffer that has not been committed yet.
  // It holds no submission id, so the in-flight comparison below cannot see
  // it, yet it references exactly the resources a release would free.
  bool has_unsubmitted_work = false;
  // The most recent submission handed to the GPU.
  uint64_t last_submitted_submission = 0;
  // The highest submission the GPU has reported complete.
  uint64_t completed_submission = 0;
  // Memexport results or render-target writebacks are queued and have not yet
  // been published to guest RAM.
  bool has_pending_producers = false;
  // A publication attempt at this boundary did not converge every selected
  // producer. Releasing now would discard the unpublished data, so the request
  // must wait for a boundary that succeeds. False when none was attempted.
  bool publication_failed = false;
  // Whether this call site is permitted to end the open submission and wait.
  // A call site that may not (mid-draw, inside a pass) can only defer.
  bool can_end_submission = false;
  // Whether this call site is permitted to drain pending producers to RAM.
  bool can_publish_pending = false;
};

constexpr CacheClearAdmission DecideCacheClearAdmission(
    const CacheClearAdmissionInputs& in) {
  if (!in.request_latched) {
    return CacheClearAdmission::kNoRequest;
  }
  // Step 1-2: nothing may be released while an encoder is open, encoded work
  // is still uncommitted, or a submission the GPU may still be reading from
  // has not completed.
  if (in.encoder_open || in.has_unsubmitted_work ||
      in.completed_submission < in.last_submitted_submission) {
    return in.can_end_submission ? CacheClearAdmission::kEndSubmissionFirst
                                 : CacheClearAdmission::kDefer;
  }
  // A publication that already reported failure at this boundary settles the
  // decision before any further attempt: the records it could not converge are
  // still pending, and re-running the same full-domain drain against the same
  // unconverged state would only repeat the failure and its waits within one
  // frame. Defer with the request latched and let the next boundary -- whose
  // publication runs as part of the frame anyway -- retry. Checked BEFORE the
  // pending-producer branch precisely so that retry cannot happen here.
  if (in.publication_failed) {
    return CacheClearAdmission::kDefer;
  }
  // Step 4: waiting made the drains possible; it did not perform them. A
  // completed submission with a queued writeback is exactly the state where
  // clearing would silently lose guest data.
  if (in.has_pending_producers) {
    return in.can_publish_pending ? CacheClearAdmission::kPublishPendingFirst
                                  : CacheClearAdmission::kDefer;
  }
  return CacheClearAdmission::kExecute;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_CACHE_CLEAR_ADMISSION_H_
