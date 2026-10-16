/**
 * @file        gpu/overlay.h
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
#include <rex/module.h>

namespace rex::gpu {

class IPresenter;

/** Texture sampling of an overlay texture. */
enum class OverlayTextureFilter : u8 {
  kNearest, /**< Uses the nearest texel. */
  kLinear,  /**< Interpolates between the 4 nearest texels. */
};

/** Primitive topology of an overlay draw. */
enum class OverlayPrimitive : u8 {
  kLines,     /**< Every 2 vertices or indices draw a line. */
  kTriangles, /**< Every 3 vertices or indices draw a triangle. */
};

/** A texture an overlay drawer created, sized in pixels. */
class OverlayTexture : public NonCopyable {
 public:
  virtual ~OverlayTexture() = default;

  /**
   * Returns the width.
   *
   * @return Width in pixels.
   */
  u32 GetWidth() const { return width_; }

  /**
   * Returns the height.
   *
   * @return Height in pixels.
   */
  u32 GetHeight() const { return height_; }

 protected:
  /**
   * Stores the size of the texture.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   */
  OverlayTexture(u32 width, u32 height) : width_(width), height_(height) {}

 private:
  u32 width_;
  u32 height_;
};

/** A rectangle in coordinate space units. A right edge left of the left edge draws nothing. */
struct OverlayRect {
  /** Left edge. */
  f32 left = 0.0f;
  /** Top edge. */
  f32 top = 0.0f;
  /** Right edge. */
  f32 right = 0.0f;
  /** Bottom edge. */
  f32 bottom = 0.0f;
};

/** An overlay vertex, laid out as the ImGui vertex. */
struct OverlayVertex {
  /** Position x in coordinate space units. */
  f32 x = 0.0f;
  /** Position y in coordinate space units. */
  f32 y = 0.0f;
  /** Texture coordinate u. */
  f32 u = 0.0f;
  /** Texture coordinate v. */
  f32 v = 0.0f;
  /** RGBA8 color, red in the low byte. */
  u32 color = 0;
};

/** The vertices and optional indices a batch of draws reads. */
struct OverlayBatch {
  /** The vertices. */
  std::span<const OverlayVertex> vertices;
  /** The indices, empty for draws without indices. */
  std::span<const u16> indices;
};

/** 1 draw inside a batch. */
struct OverlayDraw {
  /** Primitive topology. */
  OverlayPrimitive primitive = OverlayPrimitive::kTriangles;
  /** Index count, or vertex count for a batch without indices. */
  u32 count = 0;
  /** First index. */
  u32 index_offset = 0;
  /** Value added to each index, or the first vertex without indices. */
  i32 base_vertex = 0;
  /** The texture, null for the white texture. */
  OverlayTexture* texture = nullptr;
  /** The scissor in coordinate space units, none for the whole render target. */
  std::optional<OverlayRect> scissor;
};

/** Render target state a backend hands to overlay layers for 1 paint. */
class OverlayDrawContext : public NonCopyable {
 public:
  virtual ~OverlayDrawContext() = default;

  /**
   * Returns the render target width.
   *
   * @return Width in pixels.
   */
  u32 GetRenderTargetWidth() const { return render_target_width_; }

  /**
   * Returns the render target height.
   *
   * @return Height in pixels.
   */
  u32 GetRenderTargetHeight() const { return render_target_height_; }

  /**
   * Returns the presenter that paints this context.
   *
   * @return The presenter, or null for a context a consumer built.
   */
  IPresenter* GetPresenter() const { return presenter_; }

 protected:
  /**
   * Stores the render target.
   *
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

/** Draws overlay geometry into an OverlayDrawContext. Served by a backend or a consumer. */
REX_INTERFACE(IOverlayDrawer)
 public:
  /**
   * Creates a texture from RGBA8 pixels.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param filter Sampling filter.
   * @param repeat True to repeat outside 0 to 1, false to clamp.
   * @param rgba8 width times height RGBA8 pixels.
   * @return The texture, or an error when the device is not ready. The caller retries on a later
   *         frame.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<OverlayTexture>> CreateTexture(
      u32 width, u32 height, OverlayTextureFilter filter, bool repeat,
      std::span<const u8> rgba8) = 0;

  /**
   * Starts drawing into a context.
   *
   * @param context The render target.
   * @param width Coordinate space width, 0 for the render target width.
   * @param height Coordinate space height, 0 for the render target height.
   */
  virtual void Begin(OverlayDrawContext& context, f32 width, f32 height) = 0;

  /**
   * Uploads the geometry of a batch.
   *
   * @param batch The vertices and indices.
   */
  virtual void BeginBatch(const OverlayBatch& batch) = 0;

  /**
   * Draws from the open batch.
   *
   * @param draw The draw.
   */
  virtual void Draw(const OverlayDraw& draw) = 0;

  /** Closes the open batch. */
  virtual void EndBatch() = 0;

  /** Finishes drawing into the context. */
  virtual void End() = 0;
};

/** A layer the presenter draws each paint, in z order. Implemented by the UI tier. */
REX_INTERFACE(IOverlayLayer)
 public:
  /**
   * Draws the layer.
   *
   * @param context The render target of this paint.
   */
  virtual void Draw(OverlayDrawContext& context) = 0;
};

}  // namespace rex::gpu
