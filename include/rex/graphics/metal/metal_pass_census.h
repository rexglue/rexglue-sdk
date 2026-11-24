/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PASS_CENSUS_H_
#define XENIA_GPU_METAL_METAL_PASS_CENSUS_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "rex/graphics/metal/metal_pass_attachment_use.h"

namespace rex {
namespace graphics {
namespace metal {

// Why a draw pass (one MTLRenderCommandEncoder) ended.
enum class MetalPassEndReason : uint32_t {
  kAttachmentKey,     // the next draw's accumulated attachment key differs
  kAttachmentNeeded,  // D2: the next draw uses an attachment the pass left
                      // unbound (same key)
  kZpdRebuild,        // bounded: the first encoder after the ZPD pool exists
  kTransfer,          // PrepareTransferCommandBuffer (ownership transfer /
                      // upload / hazard blit)
  kColorResolve,      // IssueCopy color resolve
  kDepthResolve,      // IssueCopy depth resolve
  kResolveClear,      // IssueCopy resolve clear
  kSubmission,        // EndFrameEncoder (seam boundary; sub-bucketed by the
                      // SeamBoundaryReason index)
  kCount,
};

// Which caller closed the pass for a transfer (PrepareTransferCommandBuffer
// site; the kTransfer `detail`).
enum class MetalTransferSite : uint32_t {
  kUnknown,
  kHazardUpload,        // StageHazardUpload (shared-memory hazard blit)
  kTextureUpload,       // texture cache upload / untile callback
  kResolve,             // IssueCopy resolve / clear transfer
  kClearAtCreation,     // ClearTextureAtCreation
  kMsaaCompanions,      // EnsureCurrentMsaaCompanions
  kRasterViews,         // PrepareCurrentRasterViews (canonical materialize)
  kCanonicalReadback,   // PrepareCanonicalEdramForReadback
  kCanonicalResolve,    // PrepareCanonicalEdramForResolve
  kOwnershipTransfer,   // PerformTransfers
  kSnapshotRestore,     // RestoreEdramSnapshot
  kSnapshotFit,         // texture cache: fetch-sized view of a bridge snapshot
  kPresentResolve,      // presentable fallback: resolve a stale MSAA 1x copy
  kRenderTargetGrowth,  // GrowRenderTarget (copy into a taller texture)
  kCount,
};

inline const char* MetalTransferSiteName(uint32_t site) {
  switch (MetalTransferSite(site)) {
    case MetalTransferSite::kHazardUpload: return "hazard";
    case MetalTransferSite::kTextureUpload: return "texup";
    case MetalTransferSite::kResolve: return "resolve";
    case MetalTransferSite::kClearAtCreation: return "clearnew";
    case MetalTransferSite::kMsaaCompanions: return "msaacomp";
    case MetalTransferSite::kRasterViews: return "rasterviews";
    case MetalTransferSite::kCanonicalReadback: return "canonrb";
    case MetalTransferSite::kCanonicalResolve: return "canonres";
    case MetalTransferSite::kPresentResolve: return "presres";
    case MetalTransferSite::kOwnershipTransfer: return "owner";
    case MetalTransferSite::kSnapshotRestore: return "snaprestore";
    case MetalTransferSite::kSnapshotFit: return "snapfit";
    case MetalTransferSite::kRenderTargetGrowth: return "rtgrow";
    default: return "unknown";
  }
}

inline const char* MetalPassEndReasonName(MetalPassEndReason reason) {
  switch (reason) {
    case MetalPassEndReason::kAttachmentKey:
      return "key";
    case MetalPassEndReason::kAttachmentNeeded:
      return "needed";
    case MetalPassEndReason::kZpdRebuild:
      return "zpd";
    case MetalPassEndReason::kTransfer:
      return "xfer";
    case MetalPassEndReason::kColorResolve:
      return "cres";
    case MetalPassEndReason::kDepthResolve:
      return "dres";
    case MetalPassEndReason::kResolveClear:
      return "rclear";
    case MetalPassEndReason::kSubmission:
      return "submit";
    default:
      return "?";
  }
}

// OBSERVATIONAL pass census (reduce-metal-draw-pass-traffic task 0.1,
// XE_METAL_PASS_CENSUS=1). Device-free: the command processor feeds it the
// bound attachment set and each attachment's byte size at pass creation, each
// draw's attachment use, and the end reason; it aggregates the numbers the
// design's levers are decided on:
//
//  - traffic: bytes the bound attachments load + store per pass (2x their
//    size; a bound attachment is Load + Store under the legacy policy);
//  - avoidable: the traffic of bound attachments no draw in the pass could
//    read or write (lever D2's share);
//  - D2 simulation: the traffic and the extra "attachment needed" pass ends
//    the by-use binding WITH hysteresis would produce for the same draw
//    sequence (bound = union of the draws' use so far; a draw needing more
//    splits the pass once and the next segment binds the union);
//  - pass ends by reason, with how many of each resumed the SAME attachment
//    key in the very next pass (lever D3's merge candidates), and for the
//    resolve reasons whether the resolved tiles belonged to the open pass's
//    own attachments ("own") or to another target ("other").
//
// Counters are kept for the current reporting window (reset by Format) and
// for the process lifetime (never reset): macXenos is killed, not shut
// down, so the periodic line is the durable record; the headless trace dump
// reads the lifetime line from EmitLifecycleReports.
class MetalPassCensus {
 public:
  static constexpr uint32_t kSeamReasonCount = 16;
  static constexpr uint32_t kBoundSetCount = 1u << kMetalPassAttachmentCount;

  struct Totals {
    uint64_t passes = 0;
    uint64_t draws = 0;
    uint64_t traffic_bytes = 0;    // load + store of everything bound
    uint64_t avoidable_bytes = 0;  // ... of attachments no draw could touch
    uint64_t sim_bytes = 0;        // D2 by-use traffic for the same draws
    uint64_t sim_splits = 0;       // D2 extra "attachment needed" ends
    uint64_t ends[uint32_t(MetalPassEndReason::kCount)] = {};
    uint64_t resumes[uint32_t(MetalPassEndReason::kCount)] = {};
    uint64_t seam_ends[kSeamReasonCount] = {};
    uint64_t xfer_sites[kSeamReasonCount] = {};     // per MetalTransferSite
    uint64_t xfer_resumes[kSeamReasonCount] = {};   // ... that resumed the key
    uint64_t resolve_own[3] = {};    // per resolve reason - kColorResolve
    uint64_t resolve_other[3] = {};
    uint64_t bound_set_passes[kBoundSetCount] = {};
    uint64_t bound_set_avoidable[kBoundSetCount] = {};
    uint64_t frames = 0;
  };

  bool pass_open() const { return open_.active; }

  // `attachment_bytes` has kMetalPassAttachmentCount entries (depth, stencil,
  // color 0..3): the attachment's texture size in bytes (width x height x
  // bytes per pixel x samples), 0 where nothing is bound.
  void BeginPass(uint64_t attachment_key, uint32_t bound,
                 const uint64_t* attachment_bytes) {
    if (open_.active) {
      // A pass that was never explicitly ended (should not happen; the end
      // sites all report). Close it as a submission so nothing is lost.
      EndPass(MetalPassEndReason::kSubmission, 0, false);
    }
    open_ = Open{};
    open_.active = true;
    open_.key = attachment_key;
    open_.bound = bound & kMetalPassAttachmentAll;
    std::memcpy(open_.attachment_bytes, attachment_bytes,
                sizeof(open_.attachment_bytes));
    if (have_last_ && last_key_ == attachment_key) {
      ++window_.resumes[uint32_t(last_reason_)];
      ++lifetime_.resumes[uint32_t(last_reason_)];
      if (last_reason_ == MetalPassEndReason::kTransfer) {
        ++window_.xfer_resumes[last_detail_];
        ++lifetime_.xfer_resumes[last_detail_];
      }
    }
    have_last_ = false;
  }

  void NoteDraw(const MetalPassAttachmentUse& use) {
    if (!open_.active) {
      return;
    }
    ++open_.draws;
    uint32_t used = use.used() & open_.bound;
    open_.reads |= use.reads & open_.bound;
    open_.writes |= use.writes & open_.bound;
    if (used & ~open_.sim_bound) {
      if (open_.sim_bound) {
        ++open_.sim_splits;
        open_.sim_bytes += 2u * BytesOf(open_.sim_bound);
      }
      open_.sim_bound |= used;
    }
  }

  // `detail` is the SeamBoundaryReason index for kSubmission. For the three
  // resolve reasons, `resolve_touches_pass` says whether the resolved tiles
  // belong to one of the open pass's bound attachments.
  void EndPass(MetalPassEndReason reason, uint32_t detail,
               bool resolve_touches_pass) {
    if (!open_.active) {
      return;
    }
    open_.sim_bytes += 2u * BytesOf(open_.sim_bound);
    uint64_t traffic = 2u * BytesOf(open_.bound);
    uint64_t avoidable = 2u * BytesOf(open_.bound & ~(open_.reads | open_.writes));
    for (Totals* t : {&window_, &lifetime_}) {
      ++t->passes;
      t->draws += open_.draws;
      t->traffic_bytes += traffic;
      t->avoidable_bytes += avoidable;
      t->sim_bytes += open_.sim_bytes;
      t->sim_splits += open_.sim_splits;
      ++t->ends[uint32_t(reason)];
      if (reason == MetalPassEndReason::kSubmission) {
        ++t->seam_ends[std::min(detail, kSeamReasonCount - 1)];
      }
      if (reason == MetalPassEndReason::kTransfer) {
        ++t->xfer_sites[std::min(detail, kSeamReasonCount - 1)];
      }
      if (reason >= MetalPassEndReason::kColorResolve &&
          reason <= MetalPassEndReason::kResolveClear) {
        uint32_t r = uint32_t(reason) - uint32_t(MetalPassEndReason::kColorResolve);
        ++(resolve_touches_pass ? t->resolve_own : t->resolve_other)[r];
      }
      ++t->bound_set_passes[open_.bound];
      t->bound_set_avoidable[open_.bound] += avoidable;
    }
    last_key_ = open_.key;
    last_reason_ = reason;
    last_detail_ = std::min(detail, kSeamReasonCount - 1);
    have_last_ = true;
    open_.active = false;
  }

  void NoteFrame() {
    ++window_.frames;
    ++lifetime_.frames;
  }

  const Totals& window() const { return window_; }
  const Totals& lifetime() const { return lifetime_; }

  static std::string BoundSetName(uint32_t bound) {
    std::string s;
    if (bound & kMetalPassAttachmentDepth) s += 'D';
    if (bound & kMetalPassAttachmentStencil) s += 'S';
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (bound & MetalPassColorAttachmentBit(i)) {
        s += 'C';
        s += char('0' + i);
      }
    }
    return s.empty() ? std::string("none") : s;
  }

  // One line for the periodic tick (`window` = true, then the window resets)
  // or the lifetime record. Per-frame values divide by the frames counted.
  std::string Format(const char* site, bool window) {
    Totals& t = window ? window_ : lifetime_;
    double frames = double(std::max<uint64_t>(t.frames, 1));
    double mb = 1.0 / (1024.0 * 1024.0);
    std::string out = fmt::format(
        "MetalPassCensus[{}] frames={} passes/frame={:.1f} draws/pass={:.1f} "
        "traffic={:.2f} MB/frame avoidable={:.2f} MB/frame ({:.1f}%) "
        "d2_sim={:.2f} MB/frame ({:.1f}% saved, +{:.2f} splits/frame) | ends",
        site, t.frames, double(t.passes) / frames,
        double(t.draws) / double(std::max<uint64_t>(t.passes, 1)),
        double(t.traffic_bytes) * mb / frames,
        double(t.avoidable_bytes) * mb / frames,
        t.traffic_bytes ? 100.0 * double(t.avoidable_bytes) / double(t.traffic_bytes)
                        : 0.0,
        double(t.sim_bytes) * mb / frames,
        t.traffic_bytes
            ? 100.0 * (1.0 - double(t.sim_bytes) / double(t.traffic_bytes))
            : 0.0,
        double(t.sim_splits) / frames);
    for (uint32_t r = 0; r < uint32_t(MetalPassEndReason::kCount); ++r) {
      if (!t.ends[r]) {
        continue;
      }
      out += fmt::format(" {}={:.1f}(resume {:.1f})",
                         MetalPassEndReasonName(MetalPassEndReason(r)),
                         double(t.ends[r]) / frames,
                         double(t.resumes[r]) / frames);
      if (r >= uint32_t(MetalPassEndReason::kColorResolve) &&
          r <= uint32_t(MetalPassEndReason::kResolveClear)) {
        uint32_t i = r - uint32_t(MetalPassEndReason::kColorResolve);
        out += fmt::format("[own {:.1f} other {:.1f}]",
                           double(t.resolve_own[i]) / frames,
                           double(t.resolve_other[i]) / frames);
      }
    }
    bool any_seam = false;
    for (uint32_t s = 0; s < kSeamReasonCount; ++s) {
      if (t.seam_ends[s]) {
        out += fmt::format("{}seam{}={:.1f}", any_seam ? " " : " | ", s,
                           double(t.seam_ends[s]) / frames);
        any_seam = true;
      }
    }
    bool any_xfer = false;
    for (uint32_t s = 0; s < kSeamReasonCount; ++s) {
      if (t.xfer_sites[s]) {
        out += fmt::format("{}{}={:.1f}(resume {:.1f})", any_xfer ? " " : " | xfer:",
                           MetalTransferSiteName(s),
                           double(t.xfer_sites[s]) / frames,
                           double(t.xfer_resumes[s]) / frames);
        any_xfer = true;
      }
    }
    // Bound-set histogram, largest first, with each set's avoidable share.
    std::vector<std::pair<uint64_t, uint32_t>> sets;
    for (uint32_t b = 0; b < kBoundSetCount; ++b) {
      if (t.bound_set_passes[b]) {
        sets.emplace_back(t.bound_set_passes[b], b);
      }
    }
    std::sort(sets.begin(), sets.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    out += " | sets";
    uint32_t shown = 0;
    for (const auto& [count, b] : sets) {
      if (shown++ == 8) {
        out += " ...";
        break;
      }
      out += fmt::format(" {}={:.1f}(avoid {:.2f}MB)", BoundSetName(b),
                         double(count) / frames,
                         double(t.bound_set_avoidable[b]) * mb / frames);
    }
    if (window) {
      window_ = Totals{};
    }
    return out;
  }

 private:
  struct Open {
    bool active = false;
    uint64_t key = 0;
    uint32_t bound = 0;
    uint32_t draws = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t sim_bound = 0;
    uint32_t sim_splits = 0;
    uint64_t sim_bytes = 0;
    uint64_t attachment_bytes[kMetalPassAttachmentCount] = {};
  };

  uint64_t BytesOf(uint32_t set) const {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < kMetalPassAttachmentCount; ++i) {
      if (set & (1u << i)) {
        bytes += open_.attachment_bytes[i];
      }
    }
    return bytes;
  }

  Open open_;
  Totals window_;
  Totals lifetime_;
  bool have_last_ = false;
  uint64_t last_key_ = 0;
  MetalPassEndReason last_reason_ = MetalPassEndReason::kSubmission;
  uint32_t last_detail_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PASS_CENSUS_H_
