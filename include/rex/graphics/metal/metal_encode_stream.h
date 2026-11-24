/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ENCODE_STREAM_H_
#define XENIA_GPU_METAL_METAL_ENCODE_STREAM_H_

// Objective-C++ only (include from .mm files).
#import <Metal/Metal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "metal_irconverter_runtime.h"

namespace rex {
namespace graphics {
namespace metal {

// The render-encoder calls of the per-draw hot path (state, bindings,
// residency, draws), either issued directly or recorded into a byte ring and
// replayed by an encoder thread, so the Metal driver work runs on a second
// core while the command processor decides the next draw.
//
// XE_METAL_ENCODE_THREAD:
//   0 = direct (each call goes to the encoder at once).
//   1 = record + replay inline on the command processor at every Commit (the
//       recording cost alone; output must match 0).
//   2 (default) = record + replay on the encoder thread.
//
// Contract: only the command processor records. Commit() publishes the ops
// recorded so far (once per draw). Drain() returns once every published op has
// been replayed; any direct use of the encoder by the command processor (ending
// it, setVisibilityResultMode outside the stream, debug splits) must Drain()
// first. Objects passed by pointer must outlive their replay: the encoder,
// pipelines, depth-stencil states and arena/heap buffers are long-lived or
// retained by the command processor until after a Drain(); UseResource with
// retain=true holds a reference across the replay for cache-owned textures
// that a later draw's decision can evict.
class MetalEncodeStream {
 public:
  enum class Mode : uint32_t { kDirect = 0, kInline = 1, kThreaded = 2 };

  // Stage kinds for buffer bindings: vertex, fragment, object, mesh.
  enum StageKind : uint32_t {
    kStageVertex = 0,
    kStageFragment = 1,
    kStageObject = 2,
    kStageMesh = 3,
  };

  MetalEncodeStream();
  ~MetalEncodeStream();
  MetalEncodeStream(const MetalEncodeStream&) = delete;
  MetalEncodeStream& operator=(const MetalEncodeStream&) = delete;

  static Mode ModeFromEnvironment();
  void Start(Mode mode);
  void Shutdown();
  Mode mode() const { return mode_; }
  bool deferred() const { return mode_ != Mode::kDirect; }

  // The encoder subsequent ops go to (nil detaches). Unretained.
  void SetEncoder(id<MTLRenderCommandEncoder> encoder);

  // Render pass lifecycle on the replaying thread (deferred modes only): the
  // replay creates the encoder from `command_buffer` with `descriptor` (and
  // `label`, may be nil), and later ends it. The command processor then holds
  // no encoder object, and a render pass that follows another ends and begins
  // without a Drain. Metal allows one active encoder per command buffer, so
  // the command processor must Drain() after EndRenderPass() before it uses
  // `command_buffer` itself (another encoder, a commit, an event).
  void BeginRenderPass(__unsafe_unretained id<MTLCommandBuffer> command_buffer,
                       __unsafe_unretained MTLRenderPassDescriptor* descriptor,
                       __unsafe_unretained NSString* label);
  void EndRenderPass();

  void SetViewport(const MTLViewport& viewport);
  void SetDepthClipMode(MTLDepthClipMode mode);
  void SetCullMode(MTLCullMode mode);
  void SetFrontFacingWinding(MTLWinding winding);
  void SetTriangleFillMode(MTLTriangleFillMode mode);
  void SetDepthBias(float constant, float slope, float clamp);
  void SetScissorRect(const MTLScissorRect& rect);
  void SetRenderPipelineState(__unsafe_unretained id<MTLRenderPipelineState> state);
  // A pipeline another thread is still creating (deferred modes only): the
  // replay calls resolve(context), which blocks until the state exists and
  // returns it retained, or null if creation failed, in which case draws are
  // skipped until the next pipeline. `owner` keeps `context` alive until the
  // op has been replayed (the next Drain).
  using PipelineResolver = void* (*)(void* context);
  void SetRenderPipelineStateDeferred(PipelineResolver resolve, void* context,
                                      std::shared_ptr<void> owner);
  void SetBlendColor(float r, float g, float b, float a);
  void SetDepthStencilState(__unsafe_unretained id<MTLDepthStencilState> state);
  void SetStencilReferenceValues(uint32_t front, uint32_t back);
  void SetVisibilityResultMode(MTLVisibilityResultMode mode, NSUInteger offset);
  // Buffer-scope memory barrier inside the render encoder: buffer writes made
  // by the `after` stages of earlier draws become visible to the `before`
  // stages of later ones (hazard tracking only orders whole encoders).
  void BufferBarrier(MTLRenderStages after, MTLRenderStages before);
  void SetBuffer(uint32_t stage, __unsafe_unretained id<MTLBuffer> buffer, NSUInteger offset,
                 NSUInteger index);
  void SetBufferOffset(uint32_t stage, NSUInteger offset, NSUInteger index);
  void SetBytes(uint32_t stage, const void* bytes, NSUInteger length,
                NSUInteger index);
  void UseResource(__unsafe_unretained id<MTLResource> resource, MTLResourceUsage usage,
                   MTLRenderStages stages, bool retain);
  // Texture heaps (discrete AMD), at most kMaxUseHeaps per call.
  static constexpr uint32_t kMaxUseHeaps = 64;
  void UseHeaps(__unsafe_unretained const id<MTLHeap>* heaps, uint32_t count,
                MTLRenderStages stages);
  void DrawPrimitives(MTLPrimitiveType type, uint64_t vertex_start,
                      uint64_t vertex_count);
  void DrawIndexedPrimitives(MTLPrimitiveType type, uint64_t index_count,
                             MTLIndexType index_type,
                             __unsafe_unretained id<MTLBuffer> index_buffer, uint64_t index_offset,
                             uint64_t instance_count, int64_t base_vertex,
                             uint64_t base_instance);
  // IRRuntime draw helpers (they bind draw parameters, then draw).
  void IRDrawPrimitives(MTLPrimitiveType type, uint64_t vertex_start,
                        uint64_t vertex_count);
  void IRDrawIndexedPrimitives(MTLPrimitiveType type, uint64_t index_count,
                               MTLIndexType index_type,
                               __unsafe_unretained id<MTLBuffer> index_buffer,
                               uint64_t index_offset, uint64_t instance_count,
                               int64_t base_vertex, uint64_t base_instance);
  void IRDrawIndexedGeometryEmulation(
      IRRuntimePrimitiveType type, MTLIndexType index_type,
      __unsafe_unretained id<MTLBuffer> index_buffer, const IRRuntimeGeometryPipelineConfig& config,
      uint32_t instance_count, uint32_t index_count, uint32_t start_index,
      int base_vertex, uint32_t base_instance);
  void IRDrawGeometryEmulation(IRRuntimePrimitiveType type,
                               const IRRuntimeGeometryPipelineConfig& config,
                               uint32_t instance_count, uint32_t vertex_count,
                               uint32_t base_vertex, uint32_t base_instance);

  // Publishes the recorded ops (inline mode replays them here).
  void Commit();
  // Commit, then wait until the encoder thread has replayed everything.
  void Drain();

  struct Stats {
    uint64_t commits = 0;
    uint64_t drains = 0;
    uint64_t drain_waits = 0;      // drains that found work outstanding
    uint64_t drain_wait_ns = 0;    // time the command processor waited
    uint64_t full_waits = 0;       // ring full
    uint64_t bytes = 0;
    uint64_t consumer_sleeps = 0;
    uint64_t passes = 0;           // render passes begun on the replay thread
  };
  Stats TakeStats();

 private:
  enum class Op : uint16_t;
  struct OpHeader {
    Op op;
    uint16_t reserved;
    uint32_t size;  // bytes including the header, multiple of 8
  };

  void* Reserve(Op op, uint32_t payload_size);
  template <typename T>
  T* Emit(Op op) {
    return static_cast<T*>(Reserve(op, uint32_t(sizeof(T))));
  }
  // force: publish whatever is recorded and wake a parked consumer reliably
  // (Drain). Otherwise publication is batched (kPublishBytes) and the wake
  // check is a plain load; a wake lost to that race costs at most the
  // consumer's park timeout, and the next Drain always wakes it.
  void Publish(bool force);
  void Wake();
  void ReplayRange(uint64_t begin, uint64_t end);
  void ConsumerMain();

  Mode mode_ = Mode::kDirect;
  // Direct mode target, and the replay target in deferred modes (owned by the
  // replaying thread then).
  __unsafe_unretained id<MTLRenderCommandEncoder> direct_encoder_ = nil;
  __unsafe_unretained id<MTLRenderCommandEncoder> replay_encoder_ = nil;
  // The encoder BeginRenderPass created; replaying thread only.
  id<MTLRenderCommandEncoder> owned_encoder_ = nil;

  static constexpr uint64_t kCapacity = uint64_t(8) << 20;
  uint8_t* ring_ = nullptr;
  uint64_t write_pos_ = 0;      // producer-local cursor
  uint64_t published_local_ = 0;
  // Last consumed_ value the producer read: space checks reload it only when
  // the cached value says the ring is full (it is the consumer's hot line).
  uint64_t consumed_cached_ = 0;
  alignas(64) std::atomic<uint64_t> published_{0};
  alignas(64) std::atomic<uint64_t> consumed_{0};
  alignas(64) std::atomic<uint32_t> consumer_waiting_{0};
  std::atomic<uint32_t> producer_waiting_{0};
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> wake_word_{0};
  std::thread thread_;

  Stats stats_;
  std::atomic<uint64_t> consumer_sleeps_{0};
  // Producer side: owners of deferred pipeline contexts not yet known to be
  // replayed. Replay side: the last deferred pipeline failed.
  std::vector<std::shared_ptr<void>> deferred_owners_;
  bool replay_skip_draws_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ENCODE_STREAM_H_
