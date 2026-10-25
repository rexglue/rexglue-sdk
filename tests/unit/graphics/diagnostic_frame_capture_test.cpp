#include "graphics/d3d12/frame_capture.h"

#include <catch2/catch_test_macros.hpp>

#include <unordered_map>
#include <vector>

#include <rex/graphics/register_file.h>
#if defined(_WIN32)
#include <rex/graphics/d3d12/texture_cache.h>
#endif

namespace rex::graphics::d3d12 {

TEST_CASE("frame capture policy bounds the journal", "[graphics][diagnostic_frame_capture]") {
  CHECK(FrameCapturePolicy::CanAppend(0, 0, FrameCapturePolicy::kMaxBytes));
  CHECK_FALSE(FrameCapturePolicy::CanAppend(0, 1, FrameCapturePolicy::kMaxBytes));
  CHECK_FALSE(FrameCapturePolicy::CanAppend(FrameCapturePolicy::kMaxEvents, 0, 0));
  CHECK(FrameCapturePolicy::CanAppend(FrameCapturePolicy::kMaxEvents - 1,
                                      FrameCapturePolicy::kMaxBytes - 1, 1));
}

TEST_CASE("frame capture policy requires both completed guest boundaries",
          "[graphics][diagnostic_frame_capture]") {
  CHECK_FALSE(FrameCapturePolicy::IsComplete(false, true, true, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, false, true, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, false, true, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, true, false, true));
  CHECK_FALSE(FrameCapturePolicy::IsComplete(true, true, true, true, false));
  CHECK(FrameCapturePolicy::IsComplete(true, true, true, true, true));
}

TEST_CASE("frame journal accommodates registers through its event limit",
          "[graphics][diagnostic_frame_capture]") {
  const size_t registers_bytes = RegisterFile::kRegisterCount * sizeof(uint32_t);
  const size_t preceding_registers = (FrameCapturePolicy::kMaxEvents - 1) * registers_bytes;
  CHECK(FrameCapturePolicy::CanAppend(FrameCapturePolicy::kMaxEvents - 1, preceding_registers,
                                      registers_bytes));
}

TEST_CASE("owned frame journal preserves ordered state and guest shaders",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0xA5A5A5A5);
  const std::vector<uint32_t> vertex_ucode = {0x01020304, 0x05060708};
  const std::vector<uint32_t> pixel_ucode = {0x11121314};
  FrameCapture::ShaderView vertex_shader;
  vertex_shader.present = true;
  vertex_shader.hash = 0x1234;
  vertex_shader.dwords = vertex_ucode.data();
  vertex_shader.dword_count = vertex_ucode.size();
  FrameCapture::ShaderView pixel_shader;
  pixel_shader.present = true;
  pixel_shader.hash = 0x5678;
  pixel_shader.dwords = pixel_ucode.data();
  pixel_shader.dword_count = pixel_ucode.size();

  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(10, 20, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw = capture.BeginDraw(11, 21, 3, 0, false, {}, registers.data(), registers.size(),
                                  vertex_shader, pixel_shader);
    REQUIRE(draw.event_id());
    draw.SetResult(true);
  }
  {
    auto copy = capture.BeginCopy(11, 21, registers.data(), registers.size());
    REQUIRE(copy.event_id());
    copy.SetResult(true, "resolve");
  }
  REQUIRE_FALSE(
      capture.RecordSwapEnd(11, 22, 0x2000, 1280, 720, registers.data(), registers.size()).empty());

  const auto snapshot = capture.TakeOwnedFrame(11, 22);
  REQUIRE(snapshot);
  REQUIRE(snapshot->events.size() == 4);
  CHECK(snapshot->events[0].kind == diagnostic::DrawCaptureFrameEvent::Kind::kSwap);
  CHECK(snapshot->events[1].kind == diagnostic::DrawCaptureFrameEvent::Kind::kDraw);
  CHECK(snapshot->events[1].draw.guest_vertex_shader_hash == vertex_shader.hash);
  CHECK(snapshot->events[1].draw.guest_pixel_shader_hash == pixel_shader.hash);
  CHECK(snapshot->events[1].draw.draw.vertex_ucode == vertex_ucode);
  CHECK(snapshot->events[1].draw.draw.pixel_ucode == pixel_ucode);
  CHECK(snapshot->events[1].draw.draw.registers == registers);
  CHECK(snapshot->events[2].kind == diagnostic::DrawCaptureFrameEvent::Kind::kCopy);
  CHECK(snapshot->events[2].draw.draw.registers == registers);
  CHECK(snapshot->events[2].copy_mode == "resolve");
  CHECK(snapshot->events[3].kind == diagnostic::DrawCaptureFrameEvent::Kind::kSwap);
  CHECK(capture.IsIdle());
  CHECK(capture.event_count() == 0);
  CHECK(capture.bytes_used() == 0);
}

TEST_CASE("failed owned frame keeps its failure reason and journal size",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(1, 2, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw =
        capture.BeginDraw(1, 3, 4, 3, false, {}, registers.data(), registers.size(), {}, {});
    REQUIRE(draw.event_id());
    draw.SetResult(true);
  }
  capture.Abort(nullptr, "journal_bounds");
  CHECK_FALSE(capture.TakeOwnedFrame(1, 4));
  CHECK(capture.failure_reason() == "journal_bounds");
  CHECK(capture.event_count() == 2);
  CHECK(capture.bytes_used() > 0);
}

TEST_CASE("owned frame keeps execution and acquisition status separate",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(1, 2, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw =
        capture.BeginDraw(1, 3, 4, 3, false, {}, registers.data(), registers.size(), {}, {});
    REQUIRE(draw.event_id());
    draw.SetResult(true);
    draw.SetCaptureResult(diagnostic::DrawCaptureAvailability::kPartial,
                          diagnostic::DrawCaptureReason::kTextureUnresolved);
  }
  REQUIRE_FALSE(
      capture.RecordSwapEnd(1, 4, 0x2000, 1280, 720, registers.data(), registers.size()).empty());
  const auto snapshot = capture.TakeOwnedFrame(1, 4);
  REQUIRE(snapshot);
  REQUIRE(snapshot->events.size() == 3);
  const auto& event = snapshot->events[1];
  CHECK(event.success);
  CHECK(event.execution_status == diagnostic::DrawCaptureExecutionStatus::kNoEffect);
  CHECK(event.acquisition == diagnostic::DrawCaptureAvailability::kPartial);
  CHECK(event.acquisition_reason == diagnostic::DrawCaptureReason::kTextureUnresolved);
  CHECK(snapshot->acquisition == diagnostic::DrawCaptureAvailability::kPartial);
}

TEST_CASE("failed draw-copy execution is not classified as issued",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(1, 2, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw =
        capture.BeginDraw(1, 3, 8, 3, false, {}, registers.data(), registers.size(), {}, {});
    draw.MarkCopy(false);
    draw.SetResult(false, diagnostic::DrawCaptureReason::kExecutionFailure);
  }
  REQUIRE_FALSE(
      capture.RecordSwapEnd(1, 4, 0x2000, 1280, 720, registers.data(), registers.size()).empty());
  const auto snapshot = capture.TakeOwnedFrame(1, 4);
  REQUIRE(snapshot);
  REQUIRE(snapshot->events.size() == 3);
  CHECK_FALSE(snapshot->events[1].success);
  CHECK(snapshot->events[1].execution_status == diagnostic::DrawCaptureExecutionStatus::kFailed);
}

TEST_CASE("owned frame acquisition keeps the worst status and reason",
          "[graphics][diagnostic_frame_capture]") {
  std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
  FrameCapture capture;
  REQUIRE(capture.BeginOwnedFrame(1, 2, 0x1000, 1280, 720, registers.data(), registers.size()));
  {
    auto draw =
        capture.BeginDraw(1, 3, 4, 3, false, {}, registers.data(), registers.size(), {}, {});
    REQUIRE(draw.event_id());
    draw.SetResult(true);
    draw.SetCaptureResult(diagnostic::DrawCaptureAvailability::kPartial,
                          diagnostic::DrawCaptureReason::kTextureUnresolved);
  }
  {
    auto draw =
        capture.BeginDraw(1, 4, 4, 3, false, {}, registers.data(), registers.size(), {}, {});
    REQUIRE(draw.event_id());
    draw.SetResult(true);
    draw.SetCaptureResult(diagnostic::DrawCaptureAvailability::kUnavailable,
                          diagnostic::DrawCaptureReason::kReadbackBudget);
  }
  {
    auto draw =
        capture.BeginDraw(1, 5, 4, 3, false, {}, registers.data(), registers.size(), {}, {});
    REQUIRE(draw.event_id());
    draw.SetResult(true);
    draw.SetCaptureResult(diagnostic::DrawCaptureAvailability::kPartial,
                          diagnostic::DrawCaptureReason::kTextureDimension);
  }
  REQUIRE_FALSE(
      capture.RecordSwapEnd(1, 6, 0x2000, 1280, 720, registers.data(), registers.size()).empty());
  const auto snapshot = capture.TakeOwnedFrame(1, 6);
  REQUIRE(snapshot);
  CHECK(snapshot->acquisition == diagnostic::DrawCaptureAvailability::kUnavailable);
  CHECK(snapshot->acquisition_reason == diagnostic::DrawCaptureReason::kReadbackBudget);
}

#if defined(_WIN32)
TEST_CASE("texture capture identity separates generations and instances",
          "[graphics][diagnostic_frame_capture]") {
  using Identity = D3D12TextureCache::TextureCaptureIdentity;
  const Identity current{11, 4};
  const Identity same{11, 4};
  const Identity mutated{11, 5};
  const Identity recreated{12, 4};
  CHECK(current == same);
  CHECK_FALSE(current == mutated);
  CHECK_FALSE(current == recreated);
  std::unordered_map<Identity, uint32_t, Identity::Hasher> captures;
  captures.emplace(current, 7);
  CHECK(captures.at(same) == 7);
  CHECK(captures.find(mutated) == captures.end());
  CHECK(captures.find(recreated) == captures.end());
}
#endif

}  // namespace rex::graphics::d3d12
