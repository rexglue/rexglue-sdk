/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay.h>
#include <rex/version.h>
#include <imgui.h>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

DebugOverlayDialog::DebugOverlayDialog(const OverlayContext& context)
    : ImGuiDialog(&context.drawer()), context_(context) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(280, 280), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(0.5f);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    if (const auto& provider = context_.frame_stats()) {
      auto stats = provider();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    // Frame time graph
    auto ft_us = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kFrameTimeUs);
    float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    // GPU
    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kDrawCalls),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kCommandBufferStalls),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kVerticesProcessed));

    // Audio
    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kXMAFramesDecoded),
                static_cast<float>(rex::platform::GetSnapshotCounter(
                    rex::platform::CounterId::kAudioFrameLatencyUs)) /
                    1000.0f,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kBufferQueueDepth));

    // Dispatch
    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kFunctionsDispatched),
                rex::platform::GetSnapshotCounter(rex::platform::CounterId::kInterruptDispatches));

    // Threading
    ImGui::Text(
        "Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kActiveThreads),
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kAPCQueueDepth),
        rex::platform::GetSnapshotCounter(rex::platform::CounterId::kCriticalRegionContentions));

    // Caches
    auto tex_h = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kTextureCacheHits);
    auto tex_m = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kTextureCacheMisses);
    auto pip_h = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kPipelineCacheHits);
    auto pip_m = rex::platform::GetSnapshotCounter(rex::platform::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  // Build stamp watermark -- centered near bottom of screen
  auto text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
  float padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  float bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, imgui_drawer()->style().debug.muted_text);
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
