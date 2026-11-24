/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_DRAW_SKIP_ACCOUNTING_H_
#define XENIA_GPU_METAL_METAL_DRAW_SKIP_ACCOUNTING_H_

// Reason-coded accounting for draws IssueDraw declines to encode.
//
// WHY: the backend has ~35 sites that return true while rendering nothing, and
// they collapse into one aggregate counter. "Scene is dark" then costs an
// investigation that "12 skips: 7 unmappable-primitive, 5 pipeline-null" would
// not. Worse, an automatic trace trigger on "any skipped draw" is useless while
// harmless pruning (zero-extent scissor, no-effect draw) shares a counter with
// translation failures. Severity classification is what makes triggers
// possible: only actionable severities should arm a capture.
//
// WHY A POLICY TYPE: like MemexportGapReporter, this is production behavior --
// no cvar or environment variable is required for the classification to
// happen, and the type holds no Metal, register, or logging dependencies so
// conformance tests cover the contract directly and the lean L0 target can
// compile it device-free.
//
// The site tag is a compile-time string literal naming the skip site. Records
// aggregate per site: count, first draw index, and the primitive/shader
// identity at first occurrence -- enough to triage without replaying, exactly
// the fields a trace trigger needs to decide whether to fire.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace rex {
namespace graphics {
namespace metal {

// One reason per distinct kind of work needed to fix (or ignore) the skip.
enum class MetalDrawSkipReason : uint32_t {
  // The draw has no observable effect; skipping is correct behavior
  // (no bound RTs and no memexport, empty render extent, degenerate topology).
  kExpectedNoEffect = 0,
  // The guest asked for something this backend does not implement
  // (unmappable primitive type, unsupported constant banks, rejected route).
  kUnsupportedCapability,
  // Guest shader translation failed (ucode -> DXBC -> DXIL -> metallib).
  kTranslationFailure,
  // Shader translation succeeded but the host pipeline state object did not
  // materialize (negative pipeline cache entry, PSO creation failure).
  kPipelineFailure,
  // A host resource the draw needs was unavailable: upload arena exhaustion,
  // shared-memory range validation failure, encoder/backing object missing,
  // dependency convergence drain incomplete.
  kResourceFailure,
  // The guest state itself is invalid (null guest pointers, unset program).
  kInvalidGuestState,
  // The skip only exists because a diagnostic environment rollback is active
  // (XE_METAL_* A/B switches). Never actionable in production triage.
  kDiagnosticRollback,
  // A fail-closed internal invariant fired (assert_always sites). Always a
  // backend bug, never guest behavior; distinct so it can never be mistaken
  // for an unsupported capability.
  kInternalInvariant,
  // The guest tessellated with a shape the gated route does not support
  // (patch strips/fans, an unsupported mode/domain cell, an index/factor
  // stream endian the arena staging cannot reproduce). Distinct from
  // kUnsupportedCapability so tessellation triage never has to detangle
  // "route rejected the shape" from generic backend gaps
  // (port-xenios-tessellation task 4.1/4.2).
  kTessellationUnsupportedShape,
  // A host resource the tessellation route requires unconditionally was
  // never created (the tessellator-tables buffer, an init-time invariant this
  // backend controls). Always a backend/initialization bug, never guest
  // behavior; every tessellated draw fail-closed skips on it (task 4.3).
  kTessellationResourceMissing,
  // The draw's pipeline is still being created on a background thread
  // (::rex::graphics::metal::GetMetalSettings().metal_async_pipelines.Get()) and the draw may use a stand-in: a pass redrawn
  // every frame, so the next frame redraws it.
  kAsyncPipelinePending,
  kCount,
};

enum class MetalDrawSkipSeverity : uint32_t {
  kInfo = 0,     // expected; must never arm a trace trigger
  kWarning,      // visible-output risk; actionable
  kError,        // definite loss of guest work; actionable
};

constexpr MetalDrawSkipSeverity MetalDrawSkipSeverityFor(
    MetalDrawSkipReason reason) {
  switch (reason) {
    case MetalDrawSkipReason::kExpectedNoEffect:
    case MetalDrawSkipReason::kDiagnosticRollback:
    case MetalDrawSkipReason::kAsyncPipelinePending:
      return MetalDrawSkipSeverity::kInfo;
    case MetalDrawSkipReason::kUnsupportedCapability:
    case MetalDrawSkipReason::kInvalidGuestState:
    case MetalDrawSkipReason::kTessellationUnsupportedShape:
      return MetalDrawSkipSeverity::kWarning;
    case MetalDrawSkipReason::kTranslationFailure:
    case MetalDrawSkipReason::kPipelineFailure:
    case MetalDrawSkipReason::kResourceFailure:
    case MetalDrawSkipReason::kInternalInvariant:
    case MetalDrawSkipReason::kTessellationResourceMissing:
    default:
      return MetalDrawSkipSeverity::kError;
  }
}

inline const char* MetalDrawSkipReasonName(MetalDrawSkipReason reason) {
  switch (reason) {
    case MetalDrawSkipReason::kExpectedNoEffect:
      return "expected-no-effect";
    case MetalDrawSkipReason::kUnsupportedCapability:
      return "unsupported-capability";
    case MetalDrawSkipReason::kTranslationFailure:
      return "translation-failure";
    case MetalDrawSkipReason::kPipelineFailure:
      return "pipeline-failure";
    case MetalDrawSkipReason::kResourceFailure:
      return "resource-failure";
    case MetalDrawSkipReason::kInvalidGuestState:
      return "invalid-guest-state";
    case MetalDrawSkipReason::kDiagnosticRollback:
      return "diagnostic-rollback";
    case MetalDrawSkipReason::kInternalInvariant:
      return "internal-invariant";
    case MetalDrawSkipReason::kTessellationUnsupportedShape:
      return "tessellation-unsupported-shape";
    case MetalDrawSkipReason::kTessellationResourceMissing:
      return "tessellation-resource-missing";
    case MetalDrawSkipReason::kAsyncPipelinePending:
      return "async-pipeline-pending";
    default:
      return "unknown";
  }
}

class MetalDrawSkipAccounting {
 public:
  // Primitive/shader identity captured at a record's FIRST occurrence. Hashes
  // are guest ucode hashes (stable across runs); 0 when no shader is bound.
  struct SiteRecord {
    MetalDrawSkipReason reason = MetalDrawSkipReason::kCount;
    uint64_t count = 0;
    uint32_t first_draw_index = 0;
    uint32_t first_prim_type = 0;
    uint64_t first_vs_hash = 0;
    uint64_t first_ps_hash = 0;
  };

  // Returns true when this site has never been seen in the session (the
  // caller's cue for a one-shot log line). Aggregation is per site tag; the
  // reason is fixed per site, so a tag reused with a different reason keeps
  // the first reason (sites are literals -- a mismatch is a programming error).
  bool Record(const char* site, MetalDrawSkipReason reason,
              uint32_t draw_index, uint32_t prim_type, uint64_t vs_hash,
              uint64_t ps_hash) {
    bool first_in_session = false;
    Touch(session_, site, reason, draw_index, prim_type, vs_hash, ps_hash,
          &first_in_session);
    bool unused = false;
    Touch(frame_, site, reason, draw_index, prim_type, vs_hash, ps_hash,
          &unused);
    MetalDrawSkipSeverity severity = MetalDrawSkipSeverityFor(reason);
    ++frame_by_severity_[uint32_t(severity)];
    ++session_by_severity_[uint32_t(severity)];
    return first_in_session;
  }

  // Skips this frame that should be allowed to arm a trace trigger.
  uint64_t frame_actionable_count() const {
    return frame_by_severity_[uint32_t(MetalDrawSkipSeverity::kWarning)] +
           frame_by_severity_[uint32_t(MetalDrawSkipSeverity::kError)];
  }
  uint64_t frame_total_count() const {
    return frame_by_severity_[0] + frame_by_severity_[1] +
           frame_by_severity_[2];
  }
  uint64_t session_total_count() const {
    return session_by_severity_[0] + session_by_severity_[1] +
           session_by_severity_[2];
  }

  // Compact one-line-per-site report. `actionable_only` drops kInfo sites --
  // the shape wanted in the periodic swap log; the full form is for the
  // XE_METAL_SKIP_REPORT dump and trace-dump output.
  std::string FormatReport(bool frame_scope, bool actionable_only) const {
    const std::map<std::string, SiteRecord>& records =
        frame_scope ? frame_ : session_;
    std::string out;
    for (auto it = records.begin(); it != records.end(); ++it) {
      const SiteRecord& record = it->second;
      if (actionable_only && MetalDrawSkipSeverityFor(record.reason) ==
                                 MetalDrawSkipSeverity::kInfo) {
        continue;
      }
      if (!out.empty()) {
        out += "; ";
      }
      out += it->first;
      out += '(';
      out += MetalDrawSkipReasonName(record.reason);
      out += ")=";
      out += std::to_string(record.count);
      out += " first@";
      out += std::to_string(record.first_draw_index);
      out += " prim=";
      out += std::to_string(record.first_prim_type);
      char hashes[48];
      std::snprintf(hashes, sizeof(hashes), " vs=%08llX ps=%08llX",
                    static_cast<unsigned long long>(record.first_vs_hash &
                                                    0xFFFFFFFFull),
                    static_cast<unsigned long long>(record.first_ps_hash &
                                                    0xFFFFFFFFull));
      out += hashes;
    }
    return out;
  }

  const std::map<std::string, SiteRecord>& frame_records() const {
    return frame_;
  }
  const std::map<std::string, SiteRecord>& session_records() const {
    return session_;
  }

  // Frame boundary (called at swap). Session records persist for the
  // end-of-run report and the once-per-session first-sighting log.
  void EndFrame() {
    frame_.clear();
    frame_by_severity_[0] = frame_by_severity_[1] = frame_by_severity_[2] = 0;
  }

  void ResetSession() {
    EndFrame();
    session_.clear();
    session_by_severity_[0] = session_by_severity_[1] =
        session_by_severity_[2] = 0;
  }

 private:
  static void Touch(std::map<std::string, SiteRecord>& records,
                    const char* site, MetalDrawSkipReason reason,
                    uint32_t draw_index, uint32_t prim_type, uint64_t vs_hash,
                    uint64_t ps_hash, bool* out_first) {
    auto it = records.find(site);
    bool inserted = it == records.end();
    if (inserted) {
      it = records.insert(std::make_pair(std::string(site), SiteRecord()))
               .first;
    }
    *out_first = inserted;
    SiteRecord& record = it->second;
    if (inserted) {
      record.reason = reason;
      record.first_draw_index = draw_index;
      record.first_prim_type = prim_type;
      record.first_vs_hash = vs_hash;
      record.first_ps_hash = ps_hash;
    }
    ++record.count;
  }

  // std::map keeps report ordering stable run-to-run (diffable logs).
  std::map<std::string, SiteRecord> frame_;
  std::map<std::string, SiteRecord> session_;
  uint64_t frame_by_severity_[3] = {};
  uint64_t session_by_severity_[3] = {};
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_DRAW_SKIP_ACCOUNTING_H_
