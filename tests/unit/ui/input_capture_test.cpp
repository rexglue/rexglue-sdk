/**
 * @file        tests/unit/ui/input_capture_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <thread>

#include <imgui.h>

#include <rex/types.h>

#include "ui/input_capture.h"

namespace {

constexpr int kFrames = 100000;

}  // namespace

REX_TEST_CASE("Update copies the capture flags and Clear resets them", "[ui][input_capture]") {
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  ImGuiIO& io = ImGui::GetIO();

  rex::ui::InputCapture capture;
  CHECK_FALSE(capture.WantsMouse());
  CHECK_FALSE(capture.WantsKeyboard());

  io.WantCaptureMouse = true;
  io.WantCaptureKeyboard = false;
  capture.Update(io);
  CHECK(capture.WantsMouse());
  CHECK_FALSE(capture.WantsKeyboard());

  io.WantCaptureMouse = false;
  io.WantCaptureKeyboard = true;
  capture.Update(io);
  CHECK_FALSE(capture.WantsMouse());
  CHECK(capture.WantsKeyboard());

  capture.Clear();
  CHECK_FALSE(capture.WantsMouse());
  CHECK_FALSE(capture.WantsKeyboard());

  ImGui::DestroyContext(context);
}

REX_TEST_CASE("A reader without an ImGui context sees the flags of the UI thread",
              "[ui][input_capture]") {
  rex::ui::InputCapture capture;
  std::atomic<bool> done{false};
  std::atomic<rex::u32> reads{0};

  std::thread ui_thread([&capture, &done] {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    for (int frame = 0; frame < kFrames; ++frame) {
      ImGui::SetCurrentContext(context);
      io.WantCaptureMouse = (frame & 1) != 0;
      io.WantCaptureKeyboard = (frame & 1) == 0;
      capture.Update(io);
      ImGui::SetCurrentContext(nullptr);
    }
    ImGui::DestroyContext(context);
    done.store(true, std::memory_order_release);
  });

  std::thread reader([&capture, &done, &reads] {
    do {
      const bool mouse = capture.WantsMouse();
      const bool keyboard = capture.WantsKeyboard();
      static_cast<void>(mouse);
      static_cast<void>(keyboard);
      reads.fetch_add(1, std::memory_order_relaxed);
    } while (!done.load(std::memory_order_acquire));
  });

  ui_thread.join();
  reader.join();
  CHECK(reads.load() > 0);
}
