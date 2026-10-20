/**
 * HDR10 output: what the window gets (settings display_hdr,
 * display_hdr_paper_white, display_hdr_peak in src/ui/presenter.cpp).
 *
 * With display_hdr on, the Vulkan presenter asks the window system for an
 * HDR10 swapchain (VK_EXT_swapchain_colorspace: VK_COLOR_SPACE_HDR10_ST2084_EXT,
 * 10-bit). If it gets one, the picture is painted as before into an SDR canvas
 * and a last pass turns that into HDR10 (BT.2020 primaries, PQ brightness):
 * the picture keeps its SDR look at display_hdr_paper_white nits and only its
 * brightest highlights go up to display_hdr_peak; the UI stays at paper white.
 * Otherwise the window gets SDR as before. The Direct3D 12 presenter has no
 * HDR10 output.
 */

#pragma once

#include <cstdint>

namespace rex::ui {

enum class HdrOutput : int32_t {
  // display_hdr is off (or no window yet).
  kOff = 0,
  // The window gets HDR10 (BT.2020, SMPTE ST 2084).
  kActive = 1,
  // display_hdr is on, but the display / desktop offers no HDR10 format to
  // this window (HDR off in the system's display settings, a display without
  // HDR, or a Vulkan driver without VK_EXT_swapchain_colorspace).
  kNotOffered = 2,
  // display_hdr is on, but the graphics API in use has no HDR10 output
  // (Direct3D 12).
  kNotSupported = 3,
};

// What the window gets now. Any thread.
HdrOutput GetHdrOutput();
// For the presenters.
void SetHdrOutput(HdrOutput state);

}  // namespace rex::ui
