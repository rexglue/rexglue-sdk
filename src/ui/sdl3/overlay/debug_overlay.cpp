#include "ui/sdl3/overlay/debug_overlay.h"
#include "ui/sdl3/overlay_context.h"
#include <rex/version.h>
#include <imgui.h>
#include <rex/ui/color.h>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

DebugOverlayDialog::DebugOverlayDialog(const ModuleOverlayContext& context) : context_(context) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
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

    auto ft_us = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kFrameTimeUs);
    f32 ft_ms = static_cast<f32>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kDrawCalls),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kCommandBufferStalls),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kVerticesProcessed));

    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kXMAFramesDecoded),
                static_cast<f32>(rex::platform::GetSnapshotCounter(
                    rex::platform::CounterId::kAudioFrameLatencyUs)) /
                    1000.0f,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kBufferQueueDepth));

    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kFunctionsDispatched),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kInterruptDispatches));

    ImGui::Text(
        "Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kActiveThreads),
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kAPCQueueDepth),
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kCriticalRegionContentions));

    auto tex_h = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kTextureCacheHits);
    auto tex_m = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kTextureCacheMisses);
    auto pip_h = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kPipelineCacheHits);
    auto pip_m = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  auto text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
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
    ImGui::TextUnformatted(REXGLUE_BUILD_STAMP);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
