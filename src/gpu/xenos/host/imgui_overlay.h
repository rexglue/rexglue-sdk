/**
 * @file        gpu/xenos/host/imgui_overlay.h
 * @brief       Draws ImGui draw data through an overlay drawer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <deque>
#include <memory>
#include <unordered_map>

#include <rex/types.h>
#include "gpu/xenos/host/overlay.h"
#include "gpu/xenos/xenos_export.h"

struct ImDrawData;
struct ImTextureData;

namespace rex::gpu {

/**
 * Serves the texture requests of ImGui and draws its lists. Every method runs on the thread that
 * paints.
 */
class REX_GPU_XENOS_API ImGuiOverlay {
 public:
  /**
   * @param drawer The drawer that creates the textures and draws the lists. It must outlive the
   *        overlay.
   */
  explicit ImGuiOverlay(IOverlayDrawer& drawer);
  ~ImGuiOverlay();
  ImGuiOverlay(const ImGuiOverlay&) = delete;
  ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;

  /**
   * Creates, updates and destroys the textures ImGui asked for. A texture that cannot be created
   * keeps its request and is created again by the next call.
   *
   * @param draw_data The frame whose texture list is served.
   */
  void UpdateTextures(ImDrawData& draw_data);

  void Render(OverlayDrawContext& context, ImDrawData& draw_data);

  /** Destroys every texture, including the ones awaiting destruction. */
  void ReleaseTextures();

  size_t GetTextureCount() const { return textures_.size(); }

 private:
  struct RetiredTexture {
    u64 frame;
    std::unique_ptr<OverlayTexture> texture;
  };

  bool Upload(ImTextureData& texture);
  void Retire(ImTextureData& texture);

  IOverlayDrawer& drawer_;
  std::unordered_map<ImTextureData*, std::unique_ptr<OverlayTexture>> textures_;
  std::deque<RetiredTexture> retired_;
  u64 frame_ = 0;
};

}  // namespace rex::gpu
