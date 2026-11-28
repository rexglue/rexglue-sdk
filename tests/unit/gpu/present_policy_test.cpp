/**
 * @file        tests/unit/gpu/present_policy_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <string_view>
#include <thread>
#include <tuple>

#include "gpu/xenos/host/present_policy.h"

using namespace rex::gpu;
using namespace std::chrono_literals;

namespace {

struct ParsedAspect {
  bool parsed;
  u32 width;
  u32 height;
};

ParsedAspect ParseAspect(std::string_view aspect) {
  ParsedAspect result{false, 2560, 1080};
  result.parsed = TryParsePresentAspect(aspect, result.width, result.height);
  return result;
}

}  // namespace

REX_TEST_CASE("A present aspect gives its ratio as the display size", "[gpu][present]") {
  for (const auto& [text, width, height] :
       {std::tuple{"16:9", 16u, 9u}, std::tuple{"4:3", 4u, 3u}, std::tuple{"1280:720", 1280u, 720u},
        std::tuple{"016:09", 16u, 9u}}) {
    INFO(text);
    const ParsedAspect aspect = ParseAspect(text);
    CHECK(aspect.parsed);
    CHECK(aspect.width == width);
    CHECK(aspect.height == height);
  }
}

REX_TEST_CASE("An empty or invalid present aspect keeps the display size", "[gpu][present]") {
  for (const char* text : {"", "16", "16x9", "16:", ":9", "0:9", "16:0", "16:9x", "16:9:1", "-16:9",
                           "+16:9", " 16:9", "16:9 ", "16: 9", "99999999999:1", "1:99999999999"}) {
    INFO(text);
    const ParsedAspect aspect = ParseAspect(text);
    CHECK_FALSE(aspect.parsed);
    CHECK(aspect.width == 2560);
    CHECK(aspect.height == 1080);
  }
}

REX_TEST_CASE("A present aspect stays within the video mode sizes and 16:1 to 1:16",
              "[gpu][present]") {
  CHECK(ParseAspect("4095:4095").parsed);
  CHECK_FALSE(ParseAspect("4096:4096").parsed);
  CHECK_FALSE(ParseAspect("1:4096").parsed);
  CHECK(ParseAspect("16:1").parsed);
  CHECK(ParseAspect("1:16").parsed);
  CHECK_FALSE(ParseAspect("17:1").parsed);
  CHECK_FALSE(ParseAspect("1:17").parsed);
  CHECK(ParseAspect("4080:255").parsed);
  CHECK(ParseAspect("4095:256").parsed);
  CHECK_FALSE(ParseAspect("4095:255").parsed);
  CHECK_FALSE(ParseAspect("1073741824:1080").parsed);
}

REX_TEST_CASE("A 4:3 present aspect pillarboxes a 16:9 output and none leaves it unchanged",
              "[gpu][present]") {
  u32 aspect_x = 16;
  u32 aspect_y = 9;
  const PresentRect unchanged =
      ComputePresentRect(1920, 1080, aspect_x, aspect_y, 100, 100, false, true);
  CHECK(unchanged.x == 0);
  CHECK(unchanged.y == 0);
  CHECK(unchanged.width == 1920);
  CHECK(unchanged.height == 1080);

  REQUIRE(TryParsePresentAspect("4:3", aspect_x, aspect_y));
  const PresentRect pillarbox =
      ComputePresentRect(1920, 1080, aspect_x, aspect_y, 100, 100, false, true);
  CHECK(pillarbox.x == 240);
  CHECK(pillarbox.y == 0);
  CHECK(pillarbox.width == 1440);
  CHECK(pillarbox.height == 1080);

  const PresentRect filled =
      ComputePresentRect(1920, 1080, aspect_x, aspect_y, 100, 100, false, false);
  CHECK(filled.x == 0);
  CHECK(filled.width == 1920);
}

REX_TEST_CASE("Vsync presents with sync interval 1 and never tears", "[gpu][present]") {
  const PresentSync with_tearing = ChoosePresentSync(true, true);
  CHECK(with_tearing.sync_interval == 1);
  CHECK_FALSE(with_tearing.allow_tearing);
  const PresentSync without_tearing = ChoosePresentSync(true, false);
  CHECK(without_tearing.sync_interval == 1);
  CHECK_FALSE(without_tearing.allow_tearing);
}

REX_TEST_CASE("No vsync presents with sync interval 0 and tears when the swap chain allows it",
              "[gpu][present]") {
  const PresentSync tearing = ChoosePresentSync(false, true);
  CHECK(tearing.sync_interval == 0);
  CHECK(tearing.allow_tearing);
  const PresentSync plain = ChoosePresentSync(false, false);
  CHECK(plain.sync_interval == 0);
  CHECK_FALSE(plain.allow_tearing);
}

REX_TEST_CASE("Vsync picks mailbox, then immediate, then FIFO", "[gpu][present]") {
  CHECK(ChooseVulkanPresentMode(true, {true, true, true}) == VulkanPresentMode::kMailbox);
  CHECK(ChooseVulkanPresentMode(true, {true, false, true}) == VulkanPresentMode::kImmediate);
  CHECK(ChooseVulkanPresentMode(true, {false, false, true}) == VulkanPresentMode::kFifo);
  CHECK(ChooseVulkanPresentMode(true, {false, false, false}) == VulkanPresentMode::kFifo);
}

REX_TEST_CASE("No vsync picks the first allowed of immediate, mailbox and relaxed FIFO",
              "[gpu][present]") {
  CHECK(ChooseVulkanPresentMode(false, {true, true, true}) == VulkanPresentMode::kImmediate);
  CHECK(ChooseVulkanPresentMode(false, {false, true, true}) == VulkanPresentMode::kMailbox);
  CHECK(ChooseVulkanPresentMode(false, {false, false, true}) == VulkanPresentMode::kFifoRelaxed);
  CHECK(ChooseVulkanPresentMode(false, {false, false, false}) == VulkanPresentMode::kFifo);
}

REX_TEST_CASE("An overlay paint right after a paint waits for the rest of the interval",
              "[gpu][present]") {
  const auto start = std::chrono::steady_clock::time_point{} + 10s;
  CHECK(GetOverlayPaintDelay(start, start) == kOverlayPaintInterval);
  CHECK(GetOverlayPaintDelay(start + 5ms, start) == kOverlayPaintInterval - 5ms);
}

REX_TEST_CASE("An overlay paint after the interval does not wait", "[gpu][present]") {
  const auto start = std::chrono::steady_clock::time_point{} + 10s;
  CHECK(GetOverlayPaintDelay(start + kOverlayPaintInterval, start) == 0ns);
  CHECK(GetOverlayPaintDelay(start + 1s, start) == 0ns);
}

REX_TEST_CASE("Only a non zero sync interval makes the present vsync implicit", "[gpu][present]") {
  CHECK(IsVsyncImplicit(ChoosePresentSync(true, true)));
  CHECK(IsVsyncImplicit(ChoosePresentSync(true, false)));
  CHECK_FALSE(IsVsyncImplicit(ChoosePresentSync(false, true)));
  CHECK_FALSE(IsVsyncImplicit(ChoosePresentSync(false, false)));
}

REX_TEST_CASE("A paint connection is stale when the requested vsync differs", "[gpu][present]") {
  CHECK_FALSE(IsPaintConnectionVsyncStale(true, true));
  CHECK_FALSE(IsPaintConnectionVsyncStale(false, false));
  CHECK(IsPaintConnectionVsyncStale(true, false));
  CHECK(IsPaintConnectionVsyncStale(false, true));
}

REX_TEST_CASE("The pacer returns at once when a guest frame is ready", "[gpu][present]") {
  OverlayPaintPacer pacer(10s);
  pacer.SignalGuestOutput();
  const auto start = std::chrono::steady_clock::now();
  CHECK(pacer.Wait(start, false));
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK_FALSE(pacer.Wait(start - 20s, false));
}

REX_TEST_CASE("The pacer skips the wait when forced and consumes the ready flag",
              "[gpu][present]") {
  OverlayPaintPacer pacer(10s);
  pacer.SignalGuestOutput();
  const auto start = std::chrono::steady_clock::now();
  CHECK(pacer.Wait(start, true));
  CHECK_FALSE(pacer.Wait(start, true));
  CHECK(std::chrono::steady_clock::now() - start < 1s);
}

REX_TEST_CASE("The pacer does not wait once the interval has passed", "[gpu][present]") {
  OverlayPaintPacer pacer(10s);
  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(pacer.Wait(start - 11s, false));
  CHECK(std::chrono::steady_clock::now() - start < 1s);
}

REX_TEST_CASE("A guest frame wakes the pacer before the interval ends", "[gpu][present]") {
  OverlayPaintPacer pacer(10s);
  const auto start = std::chrono::steady_clock::now();
  std::thread signaler([&pacer] {
    std::this_thread::sleep_for(20ms);
    pacer.SignalGuestOutput();
  });
  const bool pending = pacer.Wait(start, false);
  signaler.join();
  CHECK(pending);
  CHECK(std::chrono::steady_clock::now() - start < 5s);
}

REX_TEST_CASE("The pacer waits out the interval when no guest frame arrives", "[gpu][present]") {
  OverlayPaintPacer pacer(50ms);
  const auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(pacer.Wait(start, false));
  CHECK(std::chrono::steady_clock::now() - start >= 40ms);
}
