/**
 * @file        gpu/xenos/host/overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <span>

#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::gpu {

class IPresenter;

enum class OverlayTextureFilter : u8 {
  kNearest,
  kLinear,
};

enum class OverlayPrimitive : u8 {
  kLines,     /**< Every two vertices or indices draw a line. */
  kTriangles, /**< Every three vertices or indices draw a triangle. */
};

/** A texture an overlay drawer created, sized in pixels. */
class OverlayTexture : public NonCopyable {
 public:
  virtual ~OverlayTexture() = default;

  u32 GetWidth() const { return width_; }

  u32 GetHeight() const { return height_; }

 protected:
  OverlayTexture(u32 width, u32 height) : width_(width), height_(height) {}

 private:
  u32 width_;
  u32 height_;
};

/** A rectangle in coordinate space units. A right edge left of the left edge draws nothing. */
struct OverlayRect {
  f32 left = 0.0f;
  f32 top = 0.0f;
  f32 right = 0.0f;
  f32 bottom = 0.0f;
};

/** An overlay vertex, laid out as the ImGui vertex. */
struct OverlayVertex {
  f32 x = 0.0f; /**< Position x in coordinate space units. */
  f32 y = 0.0f; /**< Position y in coordinate space units. */
  f32 u = 0.0f;
  f32 v = 0.0f;
  u32 color = 0; /**< RGBA8 color, red in the low byte. */
};

/** The vertices and optional indices a batch of draws reads. */
struct OverlayBatch {
  std::span<const OverlayVertex> vertices;
  std::span<const u16> indices; /**< The indices, empty for draws without indices. */
};

struct OverlayDraw {
  OverlayPrimitive primitive = OverlayPrimitive::kTriangles;
  u32 count = 0; /**< Index count, or vertex count for a batch without indices. */
  u32 index_offset = 0;
  i32 base_vertex = 0; /**< Value added to each index, or the first vertex without indices. */
  OverlayTexture* texture = nullptr;  /**< The texture, null for the white texture. */
  std::optional<OverlayRect> scissor; /**< Scissor in coordinate space units, or none. */
};

/** Render target state a driver hands to overlay layers for one paint. */
class OverlayDrawContext : public NonCopyable {
 public:
  virtual ~OverlayDrawContext() = default;

  u32 GetRenderTargetWidth() const { return render_target_width_; }

  u32 GetRenderTargetHeight() const { return render_target_height_; }

  /**
   * @return The presenter, or null for a context a consumer built.
   */
  IPresenter* GetPresenter() const { return presenter_; }

 protected:
  /**
   * @param presenter The presenter, or null.
   * @param render_target_width Width in pixels.
   * @param render_target_height Height in pixels.
   */
  OverlayDrawContext(IPresenter* presenter, u32 render_target_width, u32 render_target_height)
      : presenter_(presenter),
        render_target_width_(render_target_width),
        render_target_height_(render_target_height) {}

 private:
  IPresenter* presenter_;
  u32 render_target_width_;
  u32 render_target_height_;
};

/** Draws overlay geometry into an OverlayDrawContext. Served by a driver or a consumer. */
class IOverlayDrawer : public NonCopyable {
 public:
  virtual ~IOverlayDrawer() = default;

  /**
   * Creates a texture from RGBA8 pixels.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param filter Sampling filter.
   * @param repeat True to repeat outside 0 to 1, false to clamp.
   * @param rgba8 width times height RGBA8 pixels.
   * @return The texture, or an error when the device is not ready.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<OverlayTexture>> CreateTexture(
      u32 width, u32 height, OverlayTextureFilter filter, bool repeat,
      std::span<const u8> rgba8) = 0;

  /**
   * @param width Coordinate space width, 0 for the render target width.
   * @param height Coordinate space height, 0 for the render target height.
   */
  virtual void Begin(OverlayDrawContext& context, f32 width, f32 height) = 0;

  /** Uploads the geometry of a batch. */
  virtual void BeginBatch(const OverlayBatch& batch) = 0;

  /** Draws from the open batch. */
  virtual void Draw(const OverlayDraw& draw) = 0;

  virtual void EndBatch() = 0;
  virtual void End() = 0;
};

/** A layer the presenter draws each paint, in z order. Implemented by the UI tier. */
class IOverlayLayer : public NonCopyable {
 public:
  virtual ~IOverlayLayer() = default;

  virtual void Draw(OverlayDrawContext& context) = 0;
};

}  // namespace rex::gpu
