/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PRIMITIVE_PROCESSOR_H_
#define XENIA_GPU_METAL_METAL_PRIMITIVE_PROCESSOR_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "rex/math.h"
#include <gpu/xenos/primitive_processor.h>
#include <rex/xenos/register.h>
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Byte-swaps guest indices into a host (little-endian) copy for Metal. The
// production index-materialization primitive: the command processor calls this
// to fill the upload arena for both the inline path and the routed PP1 path
// (keyed on the ::rex::gpu::PrimitiveProcessor-normalized format/endian), and the device-
// free conformance test byte-compares against it. Kept here (not a test
// replica) so there is exactly one definition of the contract.
inline void SwapIndicesInto(void* dst, const void* src, uint32_t count,
                            xenos::IndexFormat format, xenos::Endian endian) {
  // The endian decision is hoisted out of the loops so each loop is a plain
  // swap or copy the compiler vectorizes (the per-element select kept GTA IV's
  // 5,000-draw frames ~3% of the command-processor thread in this function).
  if (format == xenos::IndexFormat::kInt16) {
    const uint16_t* s = static_cast<const uint16_t*>(src);
    uint16_t* d = static_cast<uint16_t*>(dst);
    const bool swap =
        endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32;
    if (!swap) {
      std::memcpy(d, s, size_t(count) * sizeof(uint16_t));
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      d[i] = rex::byte_swap(s[i]);
    }
  } else {
    const uint32_t* s = static_cast<const uint32_t*>(src);
    uint32_t* d = static_cast<uint32_t*>(dst);
    if (endian == xenos::Endian::kNone) {
      std::memcpy(d, s, size_t(count) * sizeof(uint32_t));
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      d[i] = rex::byte_swap(s[i]);
    }
  }
}

// Byte-swaps a HOST-CONVERTED index stream (::rex::gpu::PrimitiveProcessor
// kHostConverted) into the upload arena. The shared processor writes converted
// indices in the endianness it reports as host_shader_index_endian (guest
// order for pass-through conversions, 0xFFFF / 0xFFFFFFFF sentinels are
// endian-invariant), and Direct3D 12 / Vulkan swap them per element in the
// vertex shader with xenos::GPUSwap semantics. Metal keeps CPU-swapped index
// bytes and publishes vertex_index_endian = kNone, so this applies the exact
// per-element GPUSwap here: 32-bit streams that came from widened 16-bit
// guest indices carry a 16-bit endian (k8in16), which must swap bytes within
// the low half only, not reverse the whole dword.
inline void SwapConvertedIndicesInto(void* dst, const void* src,
                                     uint32_t count, xenos::IndexFormat format,
                                     xenos::Endian endian) {
  if (format == xenos::IndexFormat::kInt16) {
    SwapIndicesInto(dst, src, count, format, endian);
    return;
  }
  const uint32_t* s = static_cast<const uint32_t*>(src);
  uint32_t* d = static_cast<uint32_t*>(dst);
  if (endian == xenos::Endian::kNone) {
    std::memcpy(d, s, size_t(count) * sizeof(uint32_t));
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    d[i] = xenos::GPUSwap(s[i], endian);
  }
}

// PP1 routing predicate. Returns true for exactly the native, non-expanded,
// non-tessellated, restart-disabled line/triangle list/strip draws — from an
// auto-indexed or guest-DMA index source — that the shared ::rex::gpu::PrimitiveProcessor
// normalizes; every other class or source stays on the inline / MSC path. The
// register reads mirror the ones ::rex::gpu::PrimitiveProcessor::Process itself consumes,
// so the partition matches the processor's own view of the draw. Defined here
// (the production predicate) so the command processor and the device-free
// conformance test share one contract.
inline bool IsPp1Routable(const RegisterFile& regs,
                          xenos::PrimitiveType prim_type) {
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // The local IssueDraw prim_type must match the register Process reads;
  // otherwise the routed emission (which maps the register-derived
  // host_primitive_type) would disagree with the class this predicate checked.
  if (prim_type != vgt_draw_initiator.prim_type) {
    return false;
  }
  // Only the two index sources the routed bind/draw handles. Immediate-indexed
  // (and any reserved) source draws are outside the partition and stay inline;
  // routing them would enter Process only to fail back.
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  switch (prim_type) {
    case xenos::PrimitiveType::kLineList:
    case xenos::PrimitiveType::kTriangleList:
      // Lists are inherently non-restart (Process filters lists out of reset),
      // so eligible whenever not tessellated.
      break;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleStrip:
      // A restart-enabled strip would trigger primitive-reset normalization on
      // the shared path (index replacement / widening) — not behavior-neutral.
      // Those stay inline; primitive reset is a later capability slice.
      if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
        return false;
      }
      break;
    default:
      return false;
  }
  // Tessellated draws (explicit major mode + tessellation output path) make the
  // shared processor emit a domain host vertex-shader type; the inline Metal
  // path draws them as plain primitives. Exclude to stay behavior-neutral.
  // The gated tessellation route (XE_METAL_TESSELLATION, port-xenios-
  // tessellation task 4.1) is selected in IssueDraw AHEAD of this predicate
  // and calls Process directly, so with the gate on a tessellated draw never
  // reaches here; with the gate off this rejection IS today's behavior.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// PP4 fixed-sentinel routing predicate. This is intentionally disjoint from
// PP1: only guest-DMA line/triangle strips whose enabled 16-bit reset value
// normalizes to Metal's fixed 0xFFFF sentinel are admitted. No index scan or
// conversion is needed; Process returns the original DMA range and merely
// exposes the already-matching reset semantics in its result.
inline bool IsFixedResetStripRoutable(const RegisterFile& regs,
                                      xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kLineStrip &&
      prim_type != xenos::PrimitiveType::kTriangleStrip) {
    return false;
  }
  const reg::VGT_DRAW_INITIATOR draw = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (draw.prim_type != prim_type ||
      draw.source_select != xenos::SourceSelect::kDMA ||
      draw.index_size != xenos::IndexFormat::kInt16 ||
      !regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(draw.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }

  xenos::Endian endian = regs.Get<reg::VGT_DMA_SIZE>().swap_mode;
  if (endian != xenos::Endian::kNone && endian != xenos::Endian::k8in16) {
    endian = endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                             : xenos::Endian::kNone;
  }
  return xenos::GPUSwap(
             regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx,
             endian) == UINT16_MAX;
}

// PP1 view-consistency guard. ::rex::gpu::PrimitiveProcessor::Process reads the guest draw
// from the VGT registers, but IssueDraw's local inputs (index_count and the
// IndexBufferInfo) can legitimately diverge from those registers:
//   - --replay_index_window rewrites the local index base/count for trace
//     bisection WITHOUT touching the registers, so Process would act on a
//     different sub-range than the inline path;
//   - the inline path treats a DMA draw whose IndexBufferInfo->guest_base == 0
//     as non-indexed, whereas Process would bind address 0.
// Routing either case changes existing behavior. This returns true only when
// the local IssueDraw view matches the register view Process will act on, so
// those cases stay inline. It is a general view-consistency check (not keyed on
// any cvar name). The IndexBufferInfo fields are passed in so this stays a pure
// header helper shared with the device-free test.
inline bool IsPp1DrawViewConsistent(const RegisterFile& regs,
                                    uint32_t index_count, bool has_index_buffer,
                                    uint32_t ib_guest_base,
                                    xenos::IndexFormat ib_format,
                                    xenos::Endian ib_endianness,
                                    uint32_t ib_count) {
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // The draw count Process reads (VGT num_indices) must match the local count.
  if (index_count != vgt_draw_initiator.num_indices) {
    return false;
  }
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex) {
    // Auto-indexed: the inline path draws non-indexed, so any bound guest index
    // buffer is an inconsistent view.
    return !has_index_buffer;
  }
  // DMA: require a present, register-matching, nonzero aligned guest index
  // buffer. (IsPp1Routable already restricted source_select to
  // kDMA/kAutoIndex.)
  if (!has_index_buffer) {
    return false;
  }
  // Process clamps guest_draw_vertex_count to VGT_DMA_SIZE.num_words when the
  // draw asks for more indices than the DMA buffer holds; the inline Metal path
  // copies/draws the full index_count. Reject a short DMA buffer so the clamp
  // cannot silently change behavior — those draws stay inline.
  if (regs.Get<reg::VGT_DMA_SIZE>().num_words <
      vgt_draw_initiator.num_indices) {
    return false;
  }
  const uint32_t index_size_log2 =
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 ? 1u : 2u;
  const uint32_t register_guest_base =
      regs[XE_GPU_REG_VGT_DMA_BASE] & ~uint32_t((1u << index_size_log2) - 1u);
  // Nonzero (base 0 is the inline non-indexed fallback) and equal to the base
  // Process will compute (which also proves the local base is aligned).
  if (register_guest_base == 0 || ib_guest_base != register_guest_base) {
    return false;
  }
  if (ib_count != vgt_draw_initiator.num_indices ||
      ib_format != vgt_draw_initiator.index_size ||
      ib_endianness != regs.Get<reg::VGT_DMA_SIZE>().swap_mode) {
    return false;
  }
  return true;
}

// PP2-auto routing predicate. Returns true for exactly the auto-indexed
// triangle-fan draws that the shared ::rex::gpu::PrimitiveProcessor normalizes to a
// kHostBuiltinForAuto triangle list. DMA/index-buffer fans (including
// reset-enabled fans) return false and stay on the existing inline converter;
// tessellated fans stay inline too. Auto fans carry no guest index memory and
// have no primitive reset, so no reset/DMA checks are needed. Kept pure (no
// Metal types) so the command processor and the device-free test share one
// contract, mirroring IsPp1Routable without weakening it.
inline bool IsAutoFanRoutable(const RegisterFile& regs,
                              xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kTriangleFan) {
    return false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // The local prim_type must match the register Process reads.
  if (prim_type != vgt_draw_initiator.prim_type) {
    return false;
  }
  // Only auto-indexed fans: DMA (and reset) fans stay on the inline converter.
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  // Tessellated fans make Process emit a domain host vertex-shader type; the
  // inline path draws them as plain primitives. Exclude to stay
  // behavior-neutral.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// Auto-indexed quad lists, for GPUs where MSC geometry emulation of quads is
// not used (MetalCommandProcessor::convert_auto_quads_): routed to the shared
// built-in quad -> triangle-list table. Same exclusions as the fan route.
inline bool IsAutoQuadRoutable(const RegisterFile& regs,
                               xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kQuadList) {
    return false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (prim_type != vgt_draw_initiator.prim_type ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// DMA-indexed quad lists on the same devices: the command processor expands
// the swapped guest indices to a triangle list inline ((0,1,2),(0,2,3) per
// quad, the shared table's order). Same exclusions as above.
inline bool IsDmaQuadRoutable(const RegisterFile& regs,
                              xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kQuadList) {
    return false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (prim_type != vgt_draw_initiator.prim_type ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// DMA-indexed line loops without primitive reset: the command processor swaps
// the guest indices into the arena and appends the first one, drawing a host
// line strip (the closing segment), as the shared processor does for its own
// converted loops. Loops with primitive reset need a closing index per segment
// and stay unrouted (skipped, counted by the primitive census).
inline bool IsDmaLineLoopRoutable(const RegisterFile& regs,
                                  xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kLineLoop) {
    return false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (prim_type != vgt_draw_initiator.prim_type ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA) {
    return false;
  }
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// PP3-auto routing predicate. Returns true for exactly auto-indexed,
// restart-disabled, non-tessellated line loops. The shared ::rex::gpu::PrimitiveProcessor
// converts these to a non-indexed host line strip and reports the synthetic
// closing vertex through line_loop_closing_index. Guest-DMA loops stay out of
// this slice because they require host-converted-buffer lifetime, cache, and
// trace ownership that Metal does not enable here.
inline bool IsAutoLineLoopRoutable(const RegisterFile& regs,
                                   xenos::PrimitiveType prim_type) {
  if (prim_type != xenos::PrimitiveType::kLineLoop) {
    return false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (prim_type != vgt_draw_initiator.prim_type ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  // Primitive reset is owned by PP4 even though auto-indexed draws have no
  // guest reset markers to scan. Keep the routing partition explicit.
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  return true;
}

// Converted-class routing predicate (2026-10-26): the draws the shared
// ::rex::gpu::PrimitiveProcessor normalizes through a HOST-CONVERTED index buffer
// (ProcessedIndexBufferType::kHostConverted), which Metal now consumes:
//   - guest-DMA triangle fans and line loops, with or without primitive
//     reset (fan -> list / loop -> strip conversion, per-segment on reset);
//   - guest-DMA quad lists on devices converting quads to triangle lists;
//   - reset-enabled line/triangle strips whose reset index is NOT the fixed
//     0xFFFF 16-bit sentinel PP4 admits: a custom 16-bit reset index
//     (replaced with 0xFFFF, or widened to 32-bit when 0xFFFF is also a real
//     vertex) and 32-bit reset indices (pre-masked to 24 bits, replaced with
//     0xFFFFFFFF).
// Reset-DISABLED 16-bit lists/strips that use 0xFFFF as a real index are not
// a routing question: PP1 already routes them, and with
// host_primitive_reset_always_enabled the processor widens them to 32-bit
// (Metal cannot disable primitive restart), returning kHostConverted.
// Auto-indexed fans/loops/quads keep their built-in-table routes
// (IsAutoFanRoutable / IsAutoLineLoopRoutable / IsAutoQuadRoutable). Same
// exclusions as every other predicate: the local prim_type must match the
// register, and tessellated draws never route here.
inline bool IsConvertedClassRoutable(const RegisterFile& regs,
                                     xenos::PrimitiveType prim_type,
                                     bool convert_quad_lists) {
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (prim_type != vgt_draw_initiator.prim_type ||
      vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  switch (prim_type) {
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kLineLoop:
      return true;
    case xenos::PrimitiveType::kQuadList:
      return convert_quad_lists;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleStrip:
      // Reset-enabled strips PP4 does not own (fixed 0xFFFF 16-bit). Process
      // decides whether the reset index is actually used; an unused one keeps
      // the stream kGuestDMA with host reset semantics, which the emission
      // also handles.
      return regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
             !IsFixedResetStripRoutable(regs, prim_type);
    default:
      return false;
  }
}

// PP1 + PP2-auto + PP3-auto + PP4-fixed slice: a real shared
// `::rex::gpu::PrimitiveProcessor` subclass
// that is the single guest-semantic NORMALIZATION authority for the routed
// native, non-expanded, non-tessellated, restart-disabled topologies (line
// list/strip, triangle list/strip) AND for auto-indexed triangle fans.
//
// The Metal backend keeps its existing index MECHANISM: the command processor
// CPU-swaps the guest indices into MetalUploadArena and binds that transient as
// the index source; it never binds the shared-memory buffer as an index source.
// Which draws reach this processor is decided externally by IsPp1Routable,
// IsAutoFanRoutable, IsAutoLineLoopRoutable, and
// IsFixedResetStripRoutable—not by the configuration. This subclass therefore:
//   - configures InitializeCommon with triangle_fans_supported = false (so the
//     shared code provisions the auto-index fan built-in table) and
//     convert_triangle_fans_first_provoking = true (so both the built-in table
//     and TriangleFanToList emit Metal's historical center-first fan order).
//     host_primitive_reset_always_enabled = true (2026-10-26) lets the shared
//     code widen reset-disabled 16-bit buffers that use 0xFFFF as a real
//     index, since Metal cannot disable primitive restart.
//   - provisions HOST-CONVERTED index buffers from a per-frame CPU pool
//     (RequestHostConvertedIndexBufferForCurrentFrame): DMA fans/loops/quads,
//     custom and 32-bit reset replacement, and the widening above all return
//     kHostConverted; the command processor swaps each draw's slice into its
//     transient upload arena (GetConvertedIndexData +
//     SwapConvertedIndicesInto) and EndFrame clears the pool with the shared
//     per-frame cache at swap.
//   - implements InitializeBuiltinIndexBuffer to own the immutable fan table on
//     the CPU (no persistent MTL resource); the command processor materializes
//     each auto-fan draw's slice into the same transient upload arena via
//     CopyBuiltinIndexData.
//   - overrides RequestGuestIndexSharedMemoryRange as a true no-op so Process
//     performs no shared-memory RequestRange for the routed set. That avoids
//     the unwanted shared-memory-buffer upload and its intra-frame hazard flush
//     (EndFrameEncoderAndWait / commit+wait). Separately: a routed Process
//     emits no trace record of its own for the routed set (auto fans read no
//     guest index memory), and the command processor's existing explicit
//     WriteMemoryRead of any DMA index range preserves capture. Distinct facts.
// PP3-auto declares line loops unsupported to activate the shared loop-to-strip
// result. PP4 routes fixed-sentinel DMA line/triangle strips without
// conversion. IsConvertedClassRoutable routes DMA/reset loops, DMA/reset
// triangle fans, DMA quads (converting devices), custom/32-bit reset strips
// through host-converted buffers; reset-disabled maximum protection comes
// from the widening above. Point/rectangle expansion (vertex-shader point
// strips, MSC mesh shaders) stays on its existing paths. Tessellated draws are consumed by the
// dedicated XE_METAL_TESSELLATION route ahead of every predicate here (gate
// on), or keep their historical inline behavior (gate off) — the four
// tessellation rejections in the predicates are what keeps the non-routed
// arms behavior-neutral, so they deliberately stay.
//
// Pure C++ (no Metal types): the built-in fan table is a CPU byte vector and
// the routed draws allocate only from the command processor's arena, so this
// class holds no device or MTLBuffer, which also lets the device-free
// conformance test instantiate the real production class.
class MetalPrimitiveProcessor final : public ::rex::gpu::PrimitiveProcessor {
 public:
  // Upstream's base takes the xenos settings and the write watch as well, and owns no trace writer.
  MetalPrimitiveProcessor(const ::rex::xenos::RegisterFile& register_file,
                          const ::rex::gpu::XenosSettings& settings,
                          ::rex::IPhysicalMemory& memory,
                          ::rex::IPhysicalWriteWatch& write_watch,
                          ::rex::gpu::SharedMemory& shared_memory)
      : ::rex::gpu::PrimitiveProcessor(register_file, settings, memory, write_watch,
                                      shared_memory) {}
  // No explicit destructor: ~::rex::gpu::PrimitiveProcessor already calls ShutdownCommon().

  // convert_quad_lists: provision the shared quad-list -> triangle-list
  // built-in table so auto-indexed quad lists can route (IsAutoQuadRoutable).
  bool Initialize(bool convert_quad_lists = false);

  // Copies `byte_count` bytes of the immutable built-in index data starting at
  // `handle` (a shared byte offset, i.e. the ProcessingResult
  // host_index_buffer_handle) into `dst`. Returns false if the range is out of
  // the owned buffer's bounds (the caller must then skip the draw rather than
  // read past the table). The data never escapes this processor by reference.
  bool CopyBuiltinIndexData(size_t handle, size_t byte_count, void* dst) const;

  // Copies `byte_count` bytes of this frame's HOST-CONVERTED index data
  // starting at `handle` (the ProcessingResult host_index_buffer_handle of a
  // kHostConverted result, a byte offset into the per-frame CPU pool) into
  // `dst`. Returns false if the range is out of the pool's bounds. The command
  // processor materializes the bytes into its transient upload arena per draw
  // (swapping with SwapConvertedIndicesInto), exactly like the built-in table:
  // no persistent MTL resource, and the pool's lifetime is the frame.
  bool CopyConvertedIndexData(size_t handle, size_t byte_count,
                              void* dst) const;
  // Read access to the same range (nullptr when out of bounds); the command
  // processor swaps straight from the pool into the arena without a staging
  // copy. Valid until EndFrame.
  const void* GetConvertedIndexData(size_t handle, size_t byte_count) const {
    if (handle > converted_index_data_.size() ||
        byte_count > converted_index_data_.size() - handle) {
      return nullptr;
    }
    return converted_index_data_.data() + handle;
  }

  // Frame boundary (call at swap): clears the shared per-frame conversion
  // cache and releases this frame's converted index pool. Cached results
  // reference pool offsets, so the two are cleared together.
  void EndFrame();

  // Bytes currently held by the per-frame converted pool (diagnostics/tests).
  size_t converted_pool_bytes() const { return converted_index_data_.size(); }

 protected:
  // PP2-auto: provisions the auto-index triangle-fan built-in index table. The
  // shared InitializeCommon calls this once with the full table size; the
  // processor owns the bytes (a CPU vector) and runs the shared fill callback.
  // The Metal command processor copies the needed slice into its transient
  // upload arena per draw (CopyBuiltinIndexData) — no persistent MTL resource.
  bool InitializeBuiltinIndexBuffer(
      size_t size_bytes, std::function<void(void*)> fill_callback) override;

  // Host-converted index buffers (DMA fans/loops/quads, custom and 32-bit
  // reset indices, 16-bit reset-disabled widening): allocated from a per-frame
  // CPU byte pool; the handle is the byte offset of the (SIMD-coaligned)
  // start. Process writes through the returned pointer immediately, so pool
  // growth after the call cannot invalidate anything the shared code holds.
  void* RequestHostConvertedIndexBufferForCurrentFrame(
      xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
      uint32_t coalignment_original_address,
      size_t& backend_handle_out) override;

  // PP1: Metal materializes routed guest indices in the command processor's
  // upload arena, so no shared-memory residency is needed here. A true no-op:
  // no RequestRange -> no UploadRanges -> the intra-frame hazard flush
  // (EndFrameEncoderAndWait / commit+wait) is unreachable from Process.
  bool RequestGuestIndexSharedMemoryRange(
      uint32_t guest_index_base, uint32_t guest_index_buffer_needed_bytes,
      ProcessedIndexBufferType index_buffer_type) {
    (void)guest_index_base;
    (void)guest_index_buffer_needed_bytes;
    (void)index_buffer_type;
    return true;
  }

 private:
  // Immutable built-in index table (auto-index fan expansion), filled once in
  // InitializeBuiltinIndexBuffer. Lifetime is the processor's; never handed out
  // by reference.
  std::vector<uint8_t> builtin_index_data_;
  // Per-frame host-converted index pool (see
  // RequestHostConvertedIndexBufferForCurrentFrame); cleared by EndFrame.
  std::vector<uint8_t> converted_index_data_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_PRIMITIVE_PROCESSOR_H_
