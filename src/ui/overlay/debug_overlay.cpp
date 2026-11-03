#include "ui/overlay/debug_overlay.h"
#include "ui/overlay/overlay_context.h"
#include <rex/version.h>
#include <imgui.h>

#include <rex/ui/style.h>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/platform/perf.h>
#include <cinttypes>
#include <string_view>
#endif

namespace rex::ui {

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
namespace {

i64 Snapshot(std::string_view name) {
  const auto id = rex::platform::FindCounter(name);
  return id ? rex::platform::GetSnapshotCounter(*id) : 0;
}

}  // namespace
#endif

DebugOverlayDialog::DebugOverlayDialog(const ModuleOverlayContext& context) : context_(context) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

bool DebugOverlayDialog::Draw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(280, 280), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(context_.base.ui.GetStyle().metrics.window_alpha.hud);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    if (FrameStatsProvider& provider = context_.frame_stats) {
      auto stats = provider();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    auto ft_us = rex::platform::GetSnapshotCounter(rex::platform::CounterID::kFrameTimeUs);
    f32 ft_ms = static_cast<f32>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64, Snapshot("draw_calls"),
                Snapshot("command_buffer_stalls"), Snapshot("vertices_processed"));

    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64, Snapshot("xma_frames_decoded"),
                static_cast<f32>(Snapshot("audio_frame_latency_us")) / 1000.0f,
                Snapshot("buffer_queue_depth"));

    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64, Snapshot("functions_dispatched"),
                Snapshot("interrupt_dispatches"));

    ImGui::Text("Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
                Snapshot("active_threads"), Snapshot("apc_queue_depth"),
                Snapshot("critical_region_contentions"));

    auto tex_h = Snapshot("texture_cache_hits");
    auto tex_m = Snapshot("texture_cache_misses");
    auto pip_h = Snapshot("pipeline_cache_hits");
    auto pip_m = Snapshot("pipeline_cache_misses");
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  auto text_size = ImGui::CalcTextSize(rex::GetBuildStamp());
  f32 padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  f32 bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(context_.base.ui.GetStyle().debug.muted_text));
  if (ImGui::Begin("##watermark", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(rex::GetBuildStamp());
  }
  ImGui::End();
  ImGui::PopStyleColor();
  return true;
}

}  // namespace rex::ui
