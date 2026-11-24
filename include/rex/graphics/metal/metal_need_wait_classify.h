/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_NEED_WAIT_CLASSIFY_H_
#define XENIA_GPU_METAL_METAL_NEED_WAIT_CLASSIFY_H_

// Need-based consumer waits -- the Route A classification, as a pure value
// transform (OpenSpec optimize-need-based-consumer-waits, design D1/D1c).
//
// WHAT THIS DECIDES, AND WHAT IT DELIBERATELY DOES NOT
// ----------------------------------------------------
// `ResolvePendingProducerDependencies` historically proved producer completion
// with an UNCONDITIONAL synchronous boundary (commit + wait for everything),
// because under the pre-promotion default every pending record was untagged
// (submission 0) and a record's own tag could not describe its finality. The
// async promotion inverted that population: retirement tags are ON by default,
// so a normally-registered producer now carries a real submission tag and
// `ProducerComplete` decides it without any boundary at all.
//
// This header answers ONE question for a non-empty selected set: given the
// selected producers' tags, the completed-submission high-water mark, and the
// submission the currently-open command buffer will get, must the corridor
// commit, must it wait, and for what. It decides NOTHING about publication --
// the licence to publish stays where it already is, in the shared authority
// gate (`DecideMemexportConvergenceGate`, metal_memory_authority.h), which
// decides a TAGGED record by its tag alone and refuses an UNTAGGED one that
// carries no synchronous-boundary evidence. The design's D1a audit is what
// makes that split safe: because the gate never consults evidence for a tagged
// record, the tagged paths here need NO new evidence value, and a
// classification bug that routed an untagged record onto them would fail
// CLOSED at the gate rather than publish something unproven.
//
// It is device-free and side-effect free on purpose: the four D1 cases and
// their boundary conditions (`hw == tag`, `cur == tag`) are enumerable in an L0
// battery instead of being reachable only through a live corridor. There is no
// second, test-only copy of the rule -- production consumes exactly this
// function.
//
// FAIL-CLOSED IS THE DEFAULT, NOT A BRANCH
// -----------------------------------------
// `NeedBasedWaitDecision` default-constructs to `kFallbackUntagged`, the
// pre-feature synchronous path. Every input this header cannot decide -- an
// empty selection, any untagged record, or a tag that claims to be newer than
// the command buffer being encoded -- resolves to that same value, so the
// unreachable and the undecidable both land on the status quo rather than on
// the new path. The enumerator keeps design D1c's name because untagged IS the
// case it exists for; `NeedBasedWaitFallbackCause` says which undecidable input
// a particular request actually hit, so the anomalies never hide inside the
// expected one.

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "rex/graphics/metal/metal_memory_authority.h"
#include "rex/graphics/metal/metal_zero_copy_overlap.h"

namespace rex {
namespace graphics {
namespace metal {

inline constexpr const char* kNeedBasedWaitsEnvVar = "XE_METAL_NEED_BASED_WAITS";

// The three outcomes of D1. There is deliberately no fourth: the commit
// decision is a BIT on the narrow wait, not a separate verdict, because
// "commit then wait" and "wait" differ only in whether the producer being
// waited for is still being encoded.
enum class NeedBasedWaitDecision : uint32_t {
  // D1 case 1 (and every undecidable input): take the pre-existing full
  // synchronous boundary, unchanged to the byte. Declared FIRST so it is the
  // zero value and therefore the default of any default-constructed result.
  kFallbackUntagged,
  // D1 case 2: every selected producer is tagged at or below the completed
  // high-water mark. No commit, no wait -- publish now.
  kPublishImmediate,
  // D1 case 3: every selected producer is tagged, but at least one is still in
  // flight. Wait for the MAXIMUM selected tag, committing the open buffer first
  // only if that buffer is where a selected producer is being encoded.
  kNarrowWait,
  // Not a value; one past the last enumerator. Every real enumerator must be
  // declared ABOVE this line so the count below follows the enum automatically.
  kCount,
};
inline constexpr uint32_t kNeedBasedWaitDecisionCount =
    static_cast<uint32_t>(NeedBasedWaitDecision::kCount);

// Every REAL decision, in declaration order; the size is DEDUCED, so adding an
// enumerator without listing it here fails the static_assert and the L0 cases
// pinned to this array have to name and classify the new value.
inline constexpr std::array kNeedBasedWaitDecisions = {
    NeedBasedWaitDecision::kFallbackUntagged,
    NeedBasedWaitDecision::kPublishImmediate,
    NeedBasedWaitDecision::kNarrowWait,
};
static_assert(kNeedBasedWaitDecisions.size() == kNeedBasedWaitDecisionCount,
              "kNeedBasedWaitDecisions must enumerate every real "
              "NeedBasedWaitDecision value exactly once");

// kCount is a bound, never an observation.
constexpr bool IsNeedBasedWaitDecisionValue(NeedBasedWaitDecision value) {
  return static_cast<uint32_t>(value) < kNeedBasedWaitDecisionCount;
}

// Exhaustive, no `default:` -- a new enumerator is a compiler warning here
// rather than an unnamed number in the evidence line.
constexpr const char* NeedBasedWaitDecisionName(NeedBasedWaitDecision value) {
  switch (value) {
    case NeedBasedWaitDecision::kFallbackUntagged:
      return "fallback_untagged";
    case NeedBasedWaitDecision::kPublishImmediate:
      return "publish_immediate";
    case NeedBasedWaitDecision::kNarrowWait:
      return "narrow_wait";
    case NeedBasedWaitDecision::kCount:
      break;  // not a value
  }
  return "unknown";
}

// The one place that decides whether a decision means "the new path runs".
// kCount and any out-of-range value fall through to false: a value the
// vocabulary does not define can never take a request off the boundary.
constexpr bool NeedBasedWaitDecisionTakesNeedPath(NeedBasedWaitDecision value) {
  switch (value) {
    case NeedBasedWaitDecision::kPublishImmediate:
    case NeedBasedWaitDecision::kNarrowWait:
      return true;
    case NeedBasedWaitDecision::kFallbackUntagged:
      return false;
    case NeedBasedWaitDecision::kCount:
      break;  // not a value
  }
  return false;
}

// Why a request fell back, so a run can say whether its fallback share is the
// EXPECTED steady one (JC2's once-per-swap untagged completion-exposure
// request, design D1b) or something the vocabulary did not anticipate.
enum class NeedBasedWaitFallbackCause : uint32_t {
  kNotFallback,  // the request took the need-based path
  // The selection was empty. Unreachable from the corridor (an empty selection
  // early-outs before classification), but representable, and it must not be
  // silently readable as "everything was already retired".
  kEmptySelection,
  // D1 case 1: at least one selected producer carries no submission tag.
  kUntaggedRecord,
  // A selected tag is strictly newer than the submission the OPEN command
  // buffer will get. That cannot happen -- the open buffer's id is the highest
  // that exists -- so it is an ambiguity, not a wait target, and it fails
  // closed to the boundary while being counted under its own name.
  kTagAheadOfOpenBuffer,
  kCount,
};
inline constexpr uint32_t kNeedBasedWaitFallbackCauseCount =
    static_cast<uint32_t>(NeedBasedWaitFallbackCause::kCount);

constexpr const char* NeedBasedWaitFallbackCauseName(
    NeedBasedWaitFallbackCause value) {
  switch (value) {
    case NeedBasedWaitFallbackCause::kNotFallback:
      return "not_fallback";
    case NeedBasedWaitFallbackCause::kEmptySelection:
      return "empty_selection";
    case NeedBasedWaitFallbackCause::kUntaggedRecord:
      return "untagged_record";
    case NeedBasedWaitFallbackCause::kTagAheadOfOpenBuffer:
      return "tag_ahead_of_open_buffer";
    case NeedBasedWaitFallbackCause::kCount:
      break;  // not a value
  }
  return "unknown";
}

// The whole classification as ONE value. The factual totals travel with the
// decision so the meter counts what the classifier actually saw rather than
// re-walking the selection with a second, potentially divergent rule.
struct NeedBasedWaitClassification {
  // Defaults to the pre-feature boundary: a default-constructed result can
  // never be read as a licence to skip it.
  NeedBasedWaitDecision decision = NeedBasedWaitDecision::kFallbackUntagged;
  NeedBasedWaitFallbackCause fallback_cause =
      NeedBasedWaitFallbackCause::kEmptySelection;
  // Meaningful only for kNarrowWait: the MAXIMUM selected tag. Waiting for the
  // max is what makes this a wait for the NEED rather than for the world --
  // every other selected tag is smaller and is therefore retired with it.
  uint64_t wait_submission = 0;
  // kNarrowWait only, and true iff some selected tag EQUALS the open buffer's
  // submission: the producer is still being encoded, so the buffer must be
  // committed or the wait could never be satisfied. False when no buffer is
  // open (`open_buffer_submission == 0`), which is the no-commit branch.
  bool must_commit_open_buffer = false;
  // Factual totals over the selection, valid for every decision.
  uint32_t selected_total = 0;
  uint32_t tagged_total = 0;
  uint32_t already_complete_total = 0;
  uint64_t max_selected_tag = 0;

  bool takes_need_path() const {
    return NeedBasedWaitDecisionTakesNeedPath(decision);
  }
};

// THE CLASSIFICATION (D1 cases 1-3, D1c contract).
//
// `selected` is the authority layer's selected-producer set, in its own
// sequence order; `completed_high_water` is the completed-submission mark the
// caller just acquired; `open_buffer_submission` is the submission the OPEN
// frame command buffer will get, or 0 when no buffer is open. The caller reads
// that last value only under its existing `frame_command_buffer_` guard -- this
// function never asks whether a buffer exists, it is TOLD, and 0 means "no
// open buffer" rather than "unknown".
//
// The whole selection is inspected before any verdict is formed. A single
// untagged record poisons the request no matter where it sits, so the untagged
// test cannot be a per-record early exit that a later tagged record could
// override.
inline NeedBasedWaitClassification ClassifyNeedBasedWait(
    const std::vector<PendingProducer>& selected, uint64_t completed_high_water,
    uint64_t open_buffer_submission) {
  NeedBasedWaitClassification out;
  out.selected_total = static_cast<uint32_t>(selected.size());
  if (selected.empty()) {
    // Not reachable from the corridor, and not silently a success either.
    out.fallback_cause = NeedBasedWaitFallbackCause::kEmptySelection;
    return out;
  }
  bool any_untagged = false;
  bool any_tag_ahead_of_open_buffer = false;
  bool any_tag_in_open_buffer = false;
  for (const PendingProducer& producer : selected) {
    const uint64_t tag = producer.submission;
    if (!tag) {
      any_untagged = true;
      continue;
    }
    ++out.tagged_total;
    if (tag > out.max_selected_tag) {
      out.max_selected_tag = tag;
    }
    if (ProducerComplete(producer, completed_high_water)) {
      // Reuses the authority layer's own predicate rather than restating
      // `tag <= hw`: the completion rule must have exactly one definition, and
      // the boundary case `hw == tag` is decided there, not here.
      ++out.already_complete_total;
    }
    if (open_buffer_submission) {
      if (tag == open_buffer_submission) {
        any_tag_in_open_buffer = true;
      } else if (tag > open_buffer_submission) {
        any_tag_ahead_of_open_buffer = true;
      }
    }
  }
  if (any_untagged) {
    // D1 case 1. Checked FIRST among the fallbacks: an untagged record is the
    // expected steady fallback, and reporting it as an anomaly instead would
    // make the ordinary case look like a defect.
    out.fallback_cause = NeedBasedWaitFallbackCause::kUntaggedRecord;
    return out;
  }
  if (any_tag_ahead_of_open_buffer) {
    // Structurally impossible; fails closed and is named so a run that hits it
    // is visibly not the case the design reasoned about.
    out.fallback_cause = NeedBasedWaitFallbackCause::kTagAheadOfOpenBuffer;
    return out;
  }
  out.fallback_cause = NeedBasedWaitFallbackCause::kNotFallback;
  if (out.already_complete_total == out.selected_total) {
    // D1 case 2. No commit, no wait: every selected tag is at or below the
    // mark the authority gate itself trusts.
    out.decision = NeedBasedWaitDecision::kPublishImmediate;
    return out;
  }
  // D1 case 3. `cur == tag` is the commit case; a tag below `cur` is in an
  // already-committed buffer and needs no commit of ours.
  out.decision = NeedBasedWaitDecision::kNarrowWait;
  out.wait_submission = out.max_selected_tag;
  out.must_commit_open_buffer = any_tag_in_open_buffer;
  return out;
}

// The engagement meter (design D3), in the `zero_copy_cost_` discipline:
// bounded saturating counters advanced AT the operation that produces them, and
// one report line at the existing lifecycle emission points.
//
// UNCONDITIONAL BY DESIGN. Unlike the zero-copy cost meter this one has no
// enable switch: the gate reads engagement from these numbers, and a counter
// that only counts when someone remembered to arm it cannot distinguish "the
// feature never engaged" from "the meter was off". `requests` therefore
// advances in BOTH flag states, giving the flag-OFF control arm a comparable
// line, while the classification counters advance only where classification
// actually ran.
//
// Counters SATURATE for the same reason the zero-copy meter's do: a wrapped
// session total would read as a small number, i.e. as "almost nothing
// happened", which is precisely the claim under test.
//
// Threading: every counting site is the GPU thread's consumer corridor, and
// every read is a GPU-thread lifecycle emission. Plain integers, matching
// SeamTimingAccumulator; nothing here allocates, logs, or calls back into the
// backend.
struct NeedBasedWaitMeter {
  // Non-empty consumer requests that reached the corridor. Counted in BOTH
  // flag states -- this is the denominator every other number is read against.
  uint64_t requests = 0;
  // Requests classified (flag ON only). Below the request count exactly by the
  // requests the flag-OFF path never classified.
  uint64_t classified = 0;
  // Verdicts. fallback_* are disjoint and sum to the fallback total.
  uint64_t fallback_untagged = 0;
  uint64_t fallback_tag_ahead = 0;
  uint64_t fallback_empty = 0;
  uint64_t publish_immediate = 0;
  uint64_t tagged_waits = 0;
  // Narrow waits that committed the open buffer first (a selected producer was
  // being encoded in it). A subset of tagged_waits.
  uint64_t commit_only = 0;
  // Narrow waits the LOCKED re-check proved actually entered the condition
  // variable (WaitForSubmission's out_did_block). Never the unlocked pre-check,
  // which races with completion and must never be reported as a stall.
  uint64_t tagged_wait_blocked = 0;
  // Narrow waits that expired instead of being satisfied. Counted apart from
  // blocked waits because a timeout is a wedged GPU, not a slow one, and it is
  // the branch that deliberately leaves an uncommitted open buffer behind
  // (design D1); a run containing any is not a healthy arm.
  uint64_t tagged_wait_timeouts = 0;
  // Selection shape, summed over classified requests.
  uint64_t selected_total = 0;
  uint64_t tagged_total = 0;
  uint64_t already_complete_total = 0;
  // Narrow-wait wall time. Totals only -- the meter states how long the corridor
  // waited, never how long it would have waited.
  uint64_t wait_ns_total = 0;
  uint64_t wait_ns_max = 0;

  static void SaturatingAdd(uint64_t& counter, uint64_t added) {
    counter = ZeroCopySaturatingAdd(counter, added);
  }

  // One non-empty consumer request reached the corridor, whatever the flag says.
  void CountRequest() { SaturatingAdd(requests, 1); }

  // The classifier ran and returned `c`. Counted here rather than at the
  // branches so the verdict totals cannot drift from the verdict that was
  // actually acted on.
  void RecordClassification(const NeedBasedWaitClassification& c) {
    SaturatingAdd(classified, 1);
    SaturatingAdd(selected_total, c.selected_total);
    SaturatingAdd(tagged_total, c.tagged_total);
    SaturatingAdd(already_complete_total, c.already_complete_total);
    switch (c.decision) {
      case NeedBasedWaitDecision::kPublishImmediate:
        SaturatingAdd(publish_immediate, 1);
        break;
      case NeedBasedWaitDecision::kNarrowWait:
        SaturatingAdd(tagged_waits, 1);
        if (c.must_commit_open_buffer) {
          SaturatingAdd(commit_only, 1);
        }
        break;
      case NeedBasedWaitDecision::kFallbackUntagged:
      case NeedBasedWaitDecision::kCount:
        switch (c.fallback_cause) {
          case NeedBasedWaitFallbackCause::kTagAheadOfOpenBuffer:
            SaturatingAdd(fallback_tag_ahead, 1);
            break;
          case NeedBasedWaitFallbackCause::kEmptySelection:
            SaturatingAdd(fallback_empty, 1);
            break;
          case NeedBasedWaitFallbackCause::kNotFallback:
          case NeedBasedWaitFallbackCause::kUntaggedRecord:
          case NeedBasedWaitFallbackCause::kCount:
            // kNotFallback beside a fallback decision is not representable from
            // ClassifyNeedBasedWait; it lands in the untagged bucket rather
            // than being dropped, because an uncounted fallback would make the
            // shares stop summing to the request count.
            SaturatingAdd(fallback_untagged, 1);
            break;
        }
        break;
    }
  }

  // One narrow wait completed (or expired). `did_block` is the locked-re-check
  // fact only; `timed_out` means WaitForSubmission returned false.
  void RecordWait(uint64_t wait_ns, bool did_block, bool timed_out) {
    SaturatingAdd(wait_ns_total, wait_ns);
    if (wait_ns > wait_ns_max) {
      wait_ns_max = wait_ns;
    }
    if (did_block) {
      SaturatingAdd(tagged_wait_blocked, 1);
    }
    if (timed_out) {
      SaturatingAdd(tagged_wait_timeouts, 1);
    }
  }

  bool AnyActivity() const { return requests != 0 || classified != 0; }

  // Engagement, as D5's arm-validity rule states it: an ON arm that never
  // published immediately and never took a narrow wait is invalid, not data.
  bool Engaged() const { return publish_immediate != 0 || tagged_waits != 0; }

  // Formats the line's body into caller storage; no allocation, no logging.
  // Returns chars written (clamped). `enabled` is carried in the line because
  // the meter is the flag's banner: an all-zero ON line and an OFF line must
  // not be the same text.
  size_t Format(char* buf, size_t cap, bool enabled) const {
    if (!cap) {
      return 0;
    }
    buf[0] = '\0';
    int written = std::snprintf(
        buf, cap,
        "flag=%s requests=%llu classified=%llu publish_immediate=%llu "
        "tagged_waits=%llu commit_only=%llu blocked=%llu timeouts=%llu "
        "fallback_untagged=%llu fallback_tag_ahead=%llu fallback_empty=%llu "
        "selected=%llu tagged=%llu already_complete=%llu "
        "wait=%lluus/max%lluus",
        enabled ? "on" : "off", (unsigned long long)requests,
        (unsigned long long)classified, (unsigned long long)publish_immediate,
        (unsigned long long)tagged_waits, (unsigned long long)commit_only,
        (unsigned long long)tagged_wait_blocked,
        (unsigned long long)tagged_wait_timeouts,
        (unsigned long long)fallback_untagged,
        (unsigned long long)fallback_tag_ahead,
        (unsigned long long)fallback_empty, (unsigned long long)selected_total,
        (unsigned long long)tagged_total,
        (unsigned long long)already_complete_total,
        (unsigned long long)(wait_ns_total / 1000),
        (unsigned long long)(wait_ns_max / 1000));
    if (written <= 0) {
      return 0;
    }
    return static_cast<size_t>(written) < cap ? static_cast<size_t>(written)
                                              : cap - 1;
  }
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_NEED_WAIT_CLASSIFY_H_
