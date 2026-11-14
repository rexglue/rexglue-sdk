/**
 * @file        tests/unit/gpu/imgui_overlay_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <span>
#include <vector>

#include <imgui.h>

#include "gpu/xenos/host/imgui_overlay.h"

namespace {

using rex::gpu::OverlayTexture;

class FakeTexture final : public OverlayTexture {
 public:
  FakeTexture(rex::u32 width, rex::u32 height) : OverlayTexture(width, height) {}
};

class FakeContext final : public rex::gpu::OverlayDrawContext {
 public:
  FakeContext() : OverlayDrawContext(nullptr, 640, 480) {}
};

class FakeDrawer final : public rex::gpu::IOverlayDrawer {
 public:
  rex::Result<std::unique_ptr<OverlayTexture>> CreateTexture(
      rex::u32 width, rex::u32 height, rex::gpu::OverlayTextureFilter, bool,
      std::span<const rex::u8> rgba8) override {
    ++create_attempts;
    if (fail_creates > 0) {
      --fail_creates;
      return rex::Err<std::unique_ptr<OverlayTexture>>(rex::ErrorCategory::kRuntime, "not ready");
    }
    last_size = rgba8.size();
    return std::unique_ptr<OverlayTexture>(std::make_unique<FakeTexture>(width, height));
  }
  void Begin(rex::gpu::OverlayDrawContext&, rex::f32 width, rex::f32 height) override {
    ++begins;
    coordinate_width = width;
    coordinate_height = height;
  }
  void BeginBatch(const rex::gpu::OverlayBatch& batch) override {
    vertex_count += batch.vertices.size();
  }
  void Draw(const rex::gpu::OverlayDraw& draw) override { draws.push_back(draw); }
  void EndBatch() override {}
  void End() override { ++ends; }

  int fail_creates = 0;
  int create_attempts = 0;
  size_t last_size = 0;
  int begins = 0;
  int ends = 0;
  rex::f32 coordinate_width = 0.0f;
  rex::f32 coordinate_height = 0.0f;
  size_t vertex_count = 0;
  std::vector<rex::gpu::OverlayDraw> draws;
};

struct Frame {
  Frame() { data.Textures = &textures; }

  ImVector<ImTextureData*> textures;
  ImDrawData data;
};

}  // namespace

REX_TEST_CASE("a texture request creates the texture and marks it ready", "[gpu][imgui]") {
  FakeDrawer drawer;
  rex::gpu::ImGuiOverlay overlay(drawer);
  ImTextureData texture;
  texture.Create(ImTextureFormat_RGBA32, 16, 8);
  texture.SetStatus(ImTextureStatus_WantCreate);
  Frame frame;
  frame.textures.push_back(&texture);

  overlay.UpdateTextures(frame.data);

  CHECK(texture.Status == ImTextureStatus_OK);
  CHECK(texture.TexID != ImTextureID_Invalid);
  CHECK(drawer.last_size == size_t{16} * 8 * 4);
  CHECK(overlay.GetTextureCount() == 1);
}

REX_TEST_CASE("a failed font texture is created again", "[gpu][imgui]") {
  FakeDrawer drawer;
  drawer.fail_creates = 2;
  rex::gpu::ImGuiOverlay overlay(drawer);
  ImTextureData texture;
  texture.Create(ImTextureFormat_RGBA32, 16, 16);
  texture.SetStatus(ImTextureStatus_WantCreate);
  Frame frame;
  frame.textures.push_back(&texture);

  overlay.UpdateTextures(frame.data);
  CHECK(texture.Status == ImTextureStatus_WantCreate);
  CHECK(overlay.GetTextureCount() == 0);
  overlay.UpdateTextures(frame.data);
  CHECK(texture.Status == ImTextureStatus_WantCreate);
  overlay.UpdateTextures(frame.data);

  CHECK(drawer.create_attempts == 3);
  CHECK(texture.Status == ImTextureStatus_OK);
  CHECK(overlay.GetTextureCount() == 1);
}

REX_TEST_CASE("an alpha texture is expanded to RGBA", "[gpu][imgui]") {
  FakeDrawer drawer;
  rex::gpu::ImGuiOverlay overlay(drawer);
  ImTextureData texture;
  texture.Create(ImTextureFormat_Alpha8, 4, 4);
  texture.SetStatus(ImTextureStatus_WantCreate);
  Frame frame;
  frame.textures.push_back(&texture);

  overlay.UpdateTextures(frame.data);

  CHECK(texture.Status == ImTextureStatus_OK);
  CHECK(drawer.last_size == size_t{4} * 4 * 4);
}

REX_TEST_CASE("an update request replaces the texture", "[gpu][imgui]") {
  FakeDrawer drawer;
  rex::gpu::ImGuiOverlay overlay(drawer);
  ImTextureData texture;
  texture.Create(ImTextureFormat_RGBA32, 8, 8);
  texture.SetStatus(ImTextureStatus_WantCreate);
  Frame frame;
  frame.textures.push_back(&texture);
  overlay.UpdateTextures(frame.data);
  const ImTextureID first = texture.TexID;

  texture.SetStatus(ImTextureStatus_WantUpdates);
  overlay.UpdateTextures(frame.data);

  CHECK(texture.Status == ImTextureStatus_OK);
  CHECK(texture.TexID != first);
  CHECK(overlay.GetTextureCount() == 1);
}

REX_TEST_CASE("a destroy request releases the texture", "[gpu][imgui]") {
  FakeDrawer drawer;
  rex::gpu::ImGuiOverlay overlay(drawer);
  ImTextureData texture;
  texture.Create(ImTextureFormat_RGBA32, 8, 8);
  texture.SetStatus(ImTextureStatus_WantCreate);
  Frame frame;
  frame.textures.push_back(&texture);
  overlay.UpdateTextures(frame.data);

  texture.WantDestroyNextFrame = true;
  texture.SetStatus(ImTextureStatus_WantDestroy);
  overlay.UpdateTextures(frame.data);

  CHECK(texture.Status == ImTextureStatus_Destroyed);
  CHECK(texture.TexID == ImTextureID_Invalid);
  CHECK(overlay.GetTextureCount() == 0);
}

REX_TEST_CASE("a frame without lists draws nothing", "[gpu][imgui]") {
  FakeDrawer drawer;
  rex::gpu::ImGuiOverlay overlay(drawer);
  Frame frame;
  frame.data.DisplaySize = ImVec2(640.0f, 480.0f);
  FakeContext context;

  overlay.Render(context, frame.data);

  CHECK(drawer.begins == 0);
}
