/**
 * @file        overlay_renderer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

struct ImDrawData;

namespace rex {

/** Draws the overlays into a frame and supports ImGui's texture updates. */
class IOverlayRenderer {
 public:
  virtual ~IOverlayRenderer() = default;

  /**
   * Draws one overlay frame and applies its texture updates.
   *
   * @param draw_data Valid only during the call.
   */
  virtual void RenderOverlays(ImDrawData& draw_data) = 0;
};

}  // namespace rex
