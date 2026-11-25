#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics {

// Running totals of host work that can make a single frame much slower than
// the frames around it. The guest frame rate log reports how much of each the
// slowest frame of every 5 s window did. Pipelines can be created on worker
// threads, hence atomics (all updates are relaxed, and they are rare events
// compared to draws).
struct FrameEvents {
  std::atomic<uint64_t> texture_loads{0};
  std::atomic<uint64_t> upload_bytes{0};  // guest memory copied to the GPU buffer
  // Reuploads skipped because the data was unchanged (content fingerprints).
  std::atomic<uint64_t> texture_loads_skipped{0};
  std::atomic<uint64_t> upload_bytes_skipped{0};
  // Texture loads of data the GPU wrote (resolves, memexport).
  std::atomic<uint64_t> texture_loads_gpu_written{0};
  // Render target ownership transfers (copies between host render targets
  // sharing EDRAM): count, EDRAM tiles, and how many have a depth destination.
  std::atomic<uint64_t> rt_transfers{0};
  std::atomic<uint64_t> rt_transfer_tiles{0};
  std::atomic<uint64_t> rt_transfers_to_depth{0};
  // Transfers into color render targets skipped because the draw overwrites
  // every pixel they would copy.
  std::atomic<uint64_t> rt_transfers_skipped{0};
  // Resolves (EDRAM to memory copies): count, EDRAM tiles dumped from host
  // render targets for them, and resolve clears.
  std::atomic<uint64_t> resolves{0};
  std::atomic<uint64_t> resolve_dump_tiles{0};
  std::atomic<uint64_t> resolve_clears{0};
  // Resolves copied straight from the host render target into the host
  // textures using the data, and how many of them skipped the copy to memory.
  std::atomic<uint64_t> resolves_direct{0};
  std::atomic<uint64_t> resolves_direct_memory_skipped{0};
  // Direct3D 9 clears drawn through a 4x view that were applied straight to
  // the render target holding the memory.
  std::atomic<uint64_t> clears_forwarded{0};
  // Draws of the second tile of a frame rendered in two tiles that were
  // dropped because the first tile's draws covered both (edram_merge_tiles).
  std::atomic<uint64_t> tile_draws_merged{0};
  // Memexport draws whose output the CPU computed: not run on the GPU at all
  // (memexport_skip_cpu_computed), or run anyway (they also rasterize, or the
  // CPU output doesn't cover their exports).
  std::atomic<uint64_t> memexport_draws_skipped{0};
  std::atomic<uint64_t> memexport_draws_cpu_kept{0};
  // Draws dropped because something failed (IssueDraw's draw_fail).
  std::atomic<uint64_t> draw_failures{0};
  // Texture fetches of draws that got the null texture (no texture could be
  // made for the fetch constant).
  std::atomic<uint64_t> missing_textures{0};
  // Guest occlusion queries (Vulkan): begun; measured on the host; of those,
  // no samples passed; reported as visible without a measurement; reported
  // visible because a render pass ended mid-query; begun twice without an end.
  std::atomic<uint64_t> occlusion_begun{0};
  std::atomic<uint64_t> occlusion_measured{0};
  std::atomic<uint64_t> occlusion_zero{0};
  std::atomic<uint64_t> occlusion_reported_visible{0};
  std::atomic<uint64_t> occlusion_split{0};
  std::atomic<uint64_t> occlusion_unpaired{0};
  std::atomic<uint64_t> shader_translations{0};
  std::atomic<uint64_t> pipelines_created{0};
};

inline FrameEvents g_frame_events;

inline void CountFrameEvent(std::atomic<uint64_t>& counter, uint64_t amount = 1) {
  counter.fetch_add(amount, std::memory_order_relaxed);
}

}  // namespace rex::graphics
