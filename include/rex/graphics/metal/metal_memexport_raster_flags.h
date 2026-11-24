/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_MEMEXPORT_RASTER_FLAGS_H_
#define XENIA_GPU_METAL_METAL_MEMEXPORT_RASTER_FLAGS_H_

// extend-metal-rasterizing-memexport-producers tasks 2.1-2.4: the
// XE_METAL_MEMEXPORT_RASTER gate parse and the PURE producer-routing decision
// for memexport draws that ALSO rasterize, as functions of their inputs so the
// L0 suite pins the whole decision matrix without a device or an environment
// (the exact conventions of metal_tessellation_flags.h /
// metal_depth_semantics_flags.h).
//
// Gate contract (design D4): unset, empty, and exact "0" are OFF; exact "1" is
// ON; anything else is kMalformed so the caller can warn once, and
// behaviorally means OFF (fail closed -- the OFF arm must be bit-identical to
// a build without the feature).
//
// WHY A PURE PREDICATE: the routing decision decides whether guest writes
// reach guest memory at all. Every "unrouted" answer must carry a distinct
// reason so the unconditional gap report (metal_memexport_gap_report.h) and
// the draw-skip accounting can name it, and so the class accounting flips
// EXACTLY when a class is actually routed -- never merely because a gate was
// enabled.

#include <cstdint>
#include <cstring>

namespace rex {
namespace graphics {
namespace metal {

enum class MetalMemexportRasterGate { kOff, kOn, kMalformed };

// Pure parse of the XE_METAL_MEMEXPORT_RASTER value (nullptr = unset).
inline MetalMemexportRasterGate ParseMetalMemexportRasterGate(
    const char* value) {
  if (!value || !value[0] || !std::strcmp(value, "0")) {
    return MetalMemexportRasterGate::kOff;
  }
  if (!std::strcmp(value, "1")) {
    return MetalMemexportRasterGate::kOn;
  }
  return MetalMemexportRasterGate::kMalformed;
}

inline bool MetalMemexportRasterGateEngaged(MetalMemexportRasterGate gate) {
  return gate == MetalMemexportRasterGate::kOn;
}

// What this backend does with a memexport producer that also has a bound
// render target. Every non-routed answer is a distinct reason.
enum class MetalMemexportRasterRoute : uint32_t {
  // Not this producer class at all: no render target is bound, so the
  // already-promoted VS-only/no-RT route owns the draw.
  kNotRasterizing = 0,
  // Routed: the ORIGINAL draw rasterizes AND exports (design D1).
  kRouted,
  // Gate off -- today's behavior exactly (rasterize, drop the exports,
  // reason-coded by the gap report). The exact rollback.
  kUnroutedGateOff,
  // Pixel-shader export with the pixel-stage gate off (::rex::graphics::metal::GetMetalSettings().metal_memexport_pixel.Get()
  // = false / XE_METAL_MEMEXPORT_PIXEL=0), or through the five-input decision
  // below, which never routes the pixel stage.
  kUnroutedPixelStage,
  // The draw is running the gated tessellation route. Tessellated producers
  // stay fail-closed/out of scope; the tessellation route itself skips such a
  // draw before it can reach here, so this is a belt-and-braces answer.
  kUnroutedTessellated,
  // The shader writes eM# but no VALID stream constant produced a range
  // (AddMemExportRanges rejected every stream). Nothing to prepare, publish,
  // or converge -- routing it would publish an empty extent.
  kUnroutedNoRanges,
};

inline const char* MetalMemexportRasterRouteName(
    MetalMemexportRasterRoute route) {
  switch (route) {
    case MetalMemexportRasterRoute::kNotRasterizing:
      return "not-rasterizing";
    case MetalMemexportRasterRoute::kRouted:
      return "routed";
    case MetalMemexportRasterRoute::kUnroutedGateOff:
      return "unrouted-gate-off";
    case MetalMemexportRasterRoute::kUnroutedPixelStage:
      return "unrouted-pixel-stage";
    case MetalMemexportRasterRoute::kUnroutedTessellated:
      return "unrouted-tessellated";
    case MetalMemexportRasterRoute::kUnroutedNoRanges:
      return "unrouted-no-ranges";
    default:
      return "unknown";
  }
}

inline bool MetalMemexportRasterRouteRoutes(MetalMemexportRasterRoute route) {
  return route == MetalMemexportRasterRoute::kRouted;
}

// The pure decision. Ordering is deliberate: the two permanent exclusions
// (pixel stage, tessellated) and the "no ranges" answer are decided BEFORE the
// gate, so their reason codes are identical in both arms and the gate never
// launders an out-of-scope producer into a routed one.
//
// The full decision, with the pixel stage (2026-10-26): a pixel shader that
// writes eM# exports through the same writable shared-memory UAV, which the
// one top-level argument buffer already exposes to the fragment stage, and
// the residency declaration already spans every stage the draw runs on. The
// pixel stage has its own gate (default on); a draw whose VERTEX shader also
// exports still needs the vertex raster gate, because binding the UAV routes
// both stages' stores together.
inline MetalMemexportRasterRoute DecideMetalMemexportRoute(
    bool vertex_gate_engaged, bool pixel_gate_engaged,
    bool render_target_bound, bool vertex_export_ranges_present,
    bool pixel_stage_exports, bool pixel_export_ranges_present,
    bool tessellated_draw) {
  if (!render_target_bound) {
    return MetalMemexportRasterRoute::kNotRasterizing;
  }
  if (pixel_stage_exports) {
    if (!pixel_gate_engaged) {
      return MetalMemexportRasterRoute::kUnroutedPixelStage;
    }
    if (!vertex_export_ranges_present && !pixel_export_ranges_present) {
      return MetalMemexportRasterRoute::kUnroutedNoRanges;
    }
    if (tessellated_draw) {
      return MetalMemexportRasterRoute::kUnroutedTessellated;
    }
    if (vertex_export_ranges_present && !vertex_gate_engaged) {
      return MetalMemexportRasterRoute::kUnroutedGateOff;
    }
    return MetalMemexportRasterRoute::kRouted;
  }
  if (!vertex_export_ranges_present) {
    return MetalMemexportRasterRoute::kUnroutedNoRanges;
  }
  if (tessellated_draw) {
    return MetalMemexportRasterRoute::kUnroutedTessellated;
  }
  if (!vertex_gate_engaged) {
    return MetalMemexportRasterRoute::kUnroutedGateOff;
  }
  return MetalMemexportRasterRoute::kRouted;
}

// The vertex-stage decision with the pixel-stage gate off: the original
// contract, kept so its matrix stays pinned (pixel-stage export never routes
// here, whatever the vertex gate says).
inline MetalMemexportRasterRoute DecideMetalMemexportRasterRoute(
    bool gate_engaged, bool render_target_bound,
    bool vertex_export_ranges_present, bool pixel_stage_exports,
    bool tessellated_draw) {
  return DecideMetalMemexportRoute(
      gate_engaged, /*pixel_gate_engaged=*/false, render_target_bound,
      vertex_export_ranges_present, pixel_stage_exports,
      /*pixel_export_ranges_present=*/false, tessellated_draw);
}

// The stage mask a routed producer's WRITABLE shared-memory UAV must be
// visible on (design D2). Verified against the compiled pipeline for the
// geometry-emulated case: MSC compiles the guest VS -- including its eM#
// stores -- as the OBJECT stage (metal_pipeline_cache.mm's geometry VS stage
// reuses `vertex->translated_binary()` verbatim), while the mesh stage is the
// GENERATED DXBC geometry shader, which declares no UAV at all. The mesh bit
// is therefore a harmless superset kept only because this backend binds one
// top-level argument buffer to both mesh-path stages. Pixel-stage exports
// write from the fragment stage, which the same argument buffer and the
// draw's residency declaration already cover.
enum MetalMemexportRasterStage : uint32_t {
  kMetalMemexportRasterStageVertex = 1u << 0,
  kMetalMemexportRasterStageObject = 1u << 1,
  kMetalMemexportRasterStageMesh = 1u << 2,
};

constexpr uint32_t GetMetalMemexportRasterStageMask(bool geometry_emulated) {
  return geometry_emulated ? (kMetalMemexportRasterStageObject |
                              kMetalMemexportRasterStageMesh)
                           : kMetalMemexportRasterStageVertex;
}

// The stage that actually executes the guest VS's export stores. Object-only
// under geometry emulation; the mesh stage runs the generated GS.
constexpr uint32_t GetMetalMemexportRasterWritingStage(bool geometry_emulated) {
  return geometry_emulated ? kMetalMemexportRasterStageObject
                           : kMetalMemexportRasterStageVertex;
}

// --- Task 2.4: the tessellation factor-stream consumer extent ---------------
//
// The gated tessellation route reads its index/FACTOR stream directly from
// guest RAM at encode time, and the count it reads is the HOST patch-stream
// count (ProcessingResult::host_draw_vertex_count), NOT the guest draw's
// index_count. The generic vertex-dependency interval is built from the guest
// count, so whenever host > guest it UNDER-COVERS the bytes actually read --
// the exact shape in which a range that looks converged can still hand the
// tessellator stale tail bytes.
//
// Pure so the boundary matrix (host > guest, host == guest, host < guest,
// zero counts, and end-of-address-space saturation) is pinned without a
// device, a register file, or a trace.
struct MetalGuestStreamExtent {
  uint32_t base = 0;
  uint32_t size = 0;
  bool valid = false;
};

inline MetalGuestStreamExtent ComputeMetalIndexStreamExtent(
    uint32_t guest_base, uint32_t element_count, bool index_32bit) {
  MetalGuestStreamExtent extent;
  if (!guest_base || !element_count) {
    // No stream, or a zero-length one: never a dependency, and never a range
    // that should enter a consumer request.
    return extent;
  }
  const uint64_t element_size = index_32bit ? 4u : 2u;
  const uint64_t end =
      uint64_t(guest_base) + uint64_t(element_count) * element_size;
  // Saturate at the end of the 32-bit guest address space rather than wrapping
  // -- a wrapped interval would silently become DISJOINT from the producer it
  // depends on, which is the failure this whole task exists to prevent.
  const uint64_t clamped_end = end > 0xFFFFFFFFull ? 0xFFFFFFFFull : end;
  if (clamped_end <= uint64_t(guest_base)) {
    return extent;
  }
  extent.base = guest_base;
  extent.size = uint32_t(clamped_end - uint64_t(guest_base));
  extent.valid = true;
  return extent;
}

// True when the tessellation route's HOST stream extent reads bytes the guest
// index_count interval does not cover -- i.e. when adding the host extent to
// the consumer request is load-bearing rather than redundant.
inline bool MetalTessFactorStreamUnderCovered(uint32_t guest_base,
                                              uint32_t guest_index_count,
                                              uint32_t host_vertex_count,
                                              bool index_32bit) {
  MetalGuestStreamExtent guest =
      ComputeMetalIndexStreamExtent(guest_base, guest_index_count, index_32bit);
  MetalGuestStreamExtent host =
      ComputeMetalIndexStreamExtent(guest_base, host_vertex_count, index_32bit);
  if (!host.valid) {
    return false;
  }
  return !guest.valid || host.size > guest.size;
}

// True when `outer` fully contains `inner`. Keeps the consumer request free of
// REDUNDANT intervals: the tessellation factor extent is added only when the
// guest-index interval does not already cover the bytes the backend reads.
//
// Measured on every VP frame, index_count == host_draw_vertex_count and the two
// extents are byte-identical, so today's path adds nothing. The addition is
// STRUCTURALLY REQUIRED but CORPUS-UNENGAGED: it is retained because it
// describes the bytes this backend ACTUALLY READS (host_draw_vertex_count
// elements), not a speculative feature -- if the two ever diverge, the request
// must follow the read, not the guest draw's index count.
inline bool MetalExtentContains(const MetalGuestStreamExtent& outer,
                                const MetalGuestStreamExtent& inner) {
  if (!outer.valid || !inner.valid) {
    return false;
  }
  const uint64_t outer_end = uint64_t(outer.base) + outer.size;
  const uint64_t inner_end = uint64_t(inner.base) + inner.size;
  return uint64_t(outer.base) <= uint64_t(inner.base) && inner_end <= outer_end;
}

// FNV-1a over guest bytes: the producer->consumer JOIN hash. Used at BOTH ends
// of the chain (the bytes convergence wrote to RAM, and the raw pre-swap bytes
// the consumer stages), so equality joins them byte-for-byte instead of
// separately observing that each changed. Lives here so the L0 suite can
// MUTATION-PIN it -- a join hash that missed a single-byte corruption would
// make the entire join argument worthless.
inline uint64_t MetalMemexportJoinHash(const void* data, uint32_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 14695981039346656037ull;
  for (uint32_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  }
  return hash;
}

// --- Fail-closed staleness audit -------------------------------------------
//
// The consumer-range mutation is what makes the vertex-dependency barrier
// select an overlapping producer and converge it. If that mutation were
// removed, under-sized, or simply failed to match, the barrier would select
// nothing and the tessellation route would read guest RAM that is still older
// than the shared buffer -- SILENTLY. Missing convergence must never degrade
// into stale consumption, so the route re-checks the invariant at the point of
// the read: if any pending memexport record still overlaps the bytes about to
// be staged, convergence demonstrably did not happen and the draw fail-closed
// skips instead of tessellating from stale factors.
//
// This is a real backstop rather than a restatement of the barrier: it keys on
// PENDING-RECORD state at the moment of the read, not on the request set
// assembled earlier, so a request-set bug cannot hide from it.
inline bool MetalGuestRangesOverlap(uint32_t a_base, uint32_t a_size,
                                    uint32_t b_base, uint32_t b_size) {
  if (!a_size || !b_size) {
    return false;
  }
  const uint64_t a_begin = a_base, a_end = uint64_t(a_base) + a_size;
  const uint64_t b_begin = b_base, b_end = uint64_t(b_base) + b_size;
  return a_begin < b_end && b_begin < a_end;
}

// True when the bytes this stream is about to stage are still owed by a
// pending memexport record -- reading them now would consume stale RAM.
inline bool MetalStreamReadWouldBeStale(const MetalGuestStreamExtent& extent,
                                        uint32_t pending_base,
                                        uint32_t pending_size) {
  if (!extent.valid) {
    return false;
  }
  return MetalGuestRangesOverlap(extent.base, extent.size, pending_base,
                                 pending_size);
}

// Fault injection for the backstop's own oracle (default kNone, set only by
// XE_METAL_MEMEXPORT_RASTER_FAULT). A safety net that is never observed firing
// is not evidence of safety, and the only honest way to observe this one is to
// break the mechanism it backs up. kNoTessRange removes the consumer-range
// mutation entirely; kUndersizeTessRange keeps it but shrinks it to the first
// element, modelling a mutation that exists but under-covers.
enum class MetalMemexportRasterFault { kNone, kNoTessRange, kUndersizeTessRange };

inline MetalMemexportRasterFault ParseMetalMemexportRasterFault(
    const char* value) {
  if (!value || !value[0] || !std::strcmp(value, "0")) {
    return MetalMemexportRasterFault::kNone;
  }
  if (!std::strcmp(value, "no-tess-range")) {
    return MetalMemexportRasterFault::kNoTessRange;
  }
  if (!std::strcmp(value, "undersize-tess-range")) {
    return MetalMemexportRasterFault::kUndersizeTessRange;
  }
  return MetalMemexportRasterFault::kNone;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_MEMEXPORT_RASTER_FLAGS_H_
