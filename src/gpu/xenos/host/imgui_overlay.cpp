#include "gpu/xenos/host/imgui_overlay.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <imgui.h>

namespace rex::gpu {

namespace {

constexpr u64 kRetireFrames = 4;

static_assert(sizeof(ImDrawVert) == sizeof(OverlayVertex));
static_assert(sizeof(ImDrawIdx) == sizeof(u16));

}  // namespace

ImGuiOverlay::ImGuiOverlay(IOverlayDrawer& drawer) : drawer_(drawer) {}

ImGuiOverlay::~ImGuiOverlay() {
  ReleaseTextures();
}

bool ImGuiOverlay::Upload(ImTextureData& texture) {
  const u32 width = static_cast<u32>(texture.Width);
  const u32 height = static_cast<u32>(texture.Height);
  if (texture.Pixels == nullptr || width == 0 || height == 0) {
    return false;
  }
  std::vector<u8> expanded;
  std::span<const u8> pixels(static_cast<const u8*>(texture.GetPixels()),
                             static_cast<size_t>(width) * height * texture.BytesPerPixel);
  if (texture.Format == ImTextureFormat_Alpha8) {
    expanded.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
      expanded[i * 4 + 0] = 0xFF;
      expanded[i * 4 + 1] = 0xFF;
      expanded[i * 4 + 2] = 0xFF;
      expanded[i * 4 + 3] = pixels[i];
    }
    pixels = expanded;
  }
  auto created = drawer_.CreateTexture(width, height, OverlayTextureFilter::kLinear, true, pixels);
  if (!created) {
    return false;
  }
  std::unique_ptr<OverlayTexture>& slot = textures_[&texture];
  if (slot) {
    retired_.push_back({frame_, std::move(slot)});
  }
  slot = std::move(*created);
  texture.SetTexID(static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(slot.get())));
  texture.SetStatus(ImTextureStatus_OK);
  return true;
}

void ImGuiOverlay::Retire(ImTextureData& texture) {
  if (auto found = textures_.find(&texture); found != textures_.end()) {
    retired_.push_back({frame_, std::move(found->second)});
    textures_.erase(found);
  }
  texture.SetTexID(ImTextureID_Invalid);
  texture.SetStatus(ImTextureStatus_Destroyed);
}

void ImGuiOverlay::UpdateTextures(ImDrawData& draw_data) {
  ++frame_;
  while (!retired_.empty() && frame_ - retired_.front().frame >= kRetireFrames) {
    retired_.pop_front();
  }
  if (draw_data.Textures == nullptr) {
    return;
  }
  for (ImTextureData* texture : *draw_data.Textures) {
    switch (texture->Status) {
      case ImTextureStatus_WantCreate:
      case ImTextureStatus_WantUpdates:
        Upload(*texture);
        break;
      case ImTextureStatus_WantDestroy:
        Retire(*texture);
        break;
      default:
        break;
    }
  }
}

void ImGuiOverlay::Render(OverlayDrawContext& context, ImDrawData& draw_data) {
  if (draw_data.CmdListsCount == 0 || draw_data.DisplaySize.x <= 0.0f ||
      draw_data.DisplaySize.y <= 0.0f) {
    return;
  }
  drawer_.Begin(context, draw_data.DisplaySize.x, draw_data.DisplaySize.y);
  for (const ImDrawList* list : draw_data.CmdLists) {
    OverlayBatch batch;
    batch.vertices =
        std::span<const OverlayVertex>(reinterpret_cast<const OverlayVertex*>(list->VtxBuffer.Data),
                                       static_cast<size_t>(list->VtxBuffer.Size));
    batch.indices =
        std::span<const u16>(list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size));
    drawer_.BeginBatch(batch);
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback != nullptr || cmd.ElemCount == 0) {
        continue;
      }
      OverlayDraw draw;
      draw.primitive = OverlayPrimitive::kTriangles;
      draw.count = cmd.ElemCount;
      draw.index_offset = cmd.IdxOffset;
      draw.base_vertex = static_cast<i32>(cmd.VtxOffset);
      const ImTextureID texture_id = cmd.GetTexID();
      draw.texture = texture_id == ImTextureID_Invalid
                         ? nullptr
                         : std::bit_cast<OverlayTexture*>(static_cast<std::uintptr_t>(texture_id));
      draw.scissor = OverlayRect{
          cmd.ClipRect.x - draw_data.DisplayPos.x, cmd.ClipRect.y - draw_data.DisplayPos.y,
          cmd.ClipRect.z - draw_data.DisplayPos.x, cmd.ClipRect.w - draw_data.DisplayPos.y};
      drawer_.Draw(draw);
    }
    drawer_.EndBatch();
  }
  drawer_.End();
}

void ImGuiOverlay::ReleaseTextures() {
  textures_.clear();
  retired_.clear();
}

}  // namespace rex::gpu
