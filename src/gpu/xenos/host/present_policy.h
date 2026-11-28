/**
 * @file        gpu/xenos/host/present_policy.h
 * @brief       Present timing and aspect decisions that need no GPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include <rex/types.h>

namespace rex::gpu {

/** Overlay only paints, which draw no new guest frame, run at most this often. */
inline constexpr std::chrono::nanoseconds kOverlayPaintInterval{16'666'667};

/** The sync interval and tearing choice of one DXGI Present. */
struct PresentSync {
  u32 sync_interval;
  bool allow_tearing;
};

/**
 * @return Sync interval 1 without tearing for vsync, otherwise interval 0 with tearing when the
 * swap chain allows it.
 */
constexpr PresentSync ChoosePresentSync(bool vsync, bool tearing_supported) {
  return vsync ? PresentSync{1, false} : PresentSync{0, tearing_supported};
}

/** Reports whether a present with this sync choice waits for the vertical blank. */
constexpr bool IsVsyncImplicit(const PresentSync& sync) {
  return sync.sync_interval != 0;
}

/** Reports whether a connection made for one vsync value no longer matches the requested value. */
constexpr bool IsPaintConnectionVsyncStale(bool connected_vsync, bool requested_vsync) {
  return connected_vsync != requested_vsync;
}

enum class VulkanPresentMode : u8 {
  kImmediate,
  kMailbox,
  kFifoRelaxed,
  kFifo,
};

/** Which non FIFO modes the surface and the cvars allow. */
struct VulkanPresentModeSupport {
  bool immediate;
  bool mailbox;
  bool fifo_relaxed;
};

/**
 * @return For vsync, the first allowed of mailbox and immediate, otherwise FIFO. Without vsync,
 * the first allowed of immediate, mailbox and relaxed FIFO, and FIFO when none is allowed.
 */
constexpr VulkanPresentMode ChooseVulkanPresentMode(bool vsync,
                                                    const VulkanPresentModeSupport& support) {
  if (vsync) {
    if (support.mailbox) {
      return VulkanPresentMode::kMailbox;
    }
    if (support.immediate) {
      return VulkanPresentMode::kImmediate;
    }
    return VulkanPresentMode::kFifo;
  }
  if (support.immediate) {
    return VulkanPresentMode::kImmediate;
  }
  if (support.mailbox) {
    return VulkanPresentMode::kMailbox;
  }
  if (support.fifo_relaxed) {
    return VulkanPresentMode::kFifoRelaxed;
  }
  return VulkanPresentMode::kFifo;
}

/** The largest term of a gpu.xenos.present_aspect ratio, the largest video mode size. */
inline constexpr u32 kMaxPresentAspectTerm = 0x0FFF;

/** How far gpu.xenos.present_aspect may be from square, 16:1 to 1:16. */
inline constexpr u32 kMaxPresentAspectSkew = 16;

/**
 * Parses gpu.xenos.present_aspect, the aspect ratio to present at, into the display size that
 * Presenter::RefreshGuestOutput takes, which accepts a ratio such as 16:9 as a size.
 *
 * @param aspect W:H with two integers from 1 to kMaxPresentAspectTerm, no further from square
 * than kMaxPresentAspectSkew, such as 16:9.
 * @return True with the ratio in width_out and height_out, false with both unchanged when aspect
 * is empty or not such a ratio.
 */
inline bool TryParsePresentAspect(std::string_view aspect, u32& width_out, u32& height_out) {
  const size_t colon = aspect.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const char* begin = aspect.data();
  const char* end = begin + aspect.size();
  u32 x = 0;
  u32 y = 0;
  const auto x_result = std::from_chars(begin, begin + colon, x);
  const auto y_result = std::from_chars(begin + colon + 1, end, y);
  if (x_result.ec != std::errc() || x_result.ptr != begin + colon || y_result.ec != std::errc() ||
      y_result.ptr != end) {
    return false;
  }
  if (x < 1 || x > kMaxPresentAspectTerm || y < 1 || y > kMaxPresentAspectTerm ||
      x > y * kMaxPresentAspectSkew || y > x * kMaxPresentAspectSkew) {
    return false;
  }
  width_out = x;
  height_out = y;
  return true;
}

/** A rectangle in surface pixels. */
struct PresentRect {
  i32 x;
  i32 y;
  u32 width;
  u32 height;
};

/**
 * Scales an unsigned value by new_scale / old_scale, rounding to nearest.
 *
 * @param value The value to scale.
 * @param new_scale The numerator.
 * @param old_scale The denominator, not zero.
 * @return The scaled value.
 */
constexpr u32 RescaleUnsigned(u32 value, u32 new_scale, u32 old_scale) {
  return static_cast<u32>((static_cast<u64>(value) * new_scale + (old_scale >> 1)) / old_scale);
}

/**
 * Scales a signed value by new_scale / old_scale, rounding to nearest away from zero.
 *
 * @param value The value to scale.
 * @param new_scale The numerator.
 * @param old_scale The denominator, not zero.
 * @return The scaled value.
 */
constexpr i32 RescaleSigned(i32 value, u32 new_scale, u32 old_scale) {
  return static_cast<i32>((static_cast<i64>(value) * new_scale +
                           static_cast<i64>(old_scale >> 1) * (value < 0 ? -1 : 1)) /
                          old_scale);
}

/**
 * Returns where the guest output sits in the surface for a display aspect ratio.
 *
 * @param surface_width The surface width, not zero.
 * @param surface_height The surface height, not zero.
 * @param aspect_x The display aspect ratio width term, not zero.
 * @param aspect_y The display aspect ratio height term, not zero.
 * @param safe_area_x The horizontal safe area percent, used when allow_overscan_cutoff is set.
 * @param safe_area_y The vertical safe area percent, used when allow_overscan_cutoff is set.
 * @param allow_overscan_cutoff True to cut the output off at the safe area.
 * @param letterbox True to keep the aspect ratio with bars, false to fill the surface.
 * @return The output rectangle, which may extend past the surface when the cutoff applies.
 */
constexpr PresentRect ComputePresentRect(u32 surface_width, u32 surface_height, u32 aspect_x,
                                         u32 aspect_y, u32 safe_area_x, u32 safe_area_y,
                                         bool allow_overscan_cutoff, bool letterbox) {
  PresentRect rect{};
  if (static_cast<u64>(surface_width) * aspect_y > static_cast<u64>(surface_height) * aspect_x) {
    u32 safe_area = 100;
    if (allow_overscan_cutoff && safe_area_y > 0 && safe_area_y < 100) {
      safe_area = safe_area_y;
    }
    rect.height = RescaleUnsigned(surface_width, aspect_y, aspect_x);
    bool bars = false;
    if (rect.height * safe_area > surface_height * 100) {
      rect.height = RescaleUnsigned(surface_height, 100, safe_area);
      bars = true;
    }
    if (bars && letterbox) {
      rect.width = RescaleUnsigned(surface_height * 100, aspect_x, aspect_y * safe_area);
      rect.x = (static_cast<i32>(surface_width) - static_cast<i32>(rect.width)) / 2;
    } else {
      rect.width = surface_width;
    }
    rect.y = (static_cast<i32>(surface_height) - static_cast<i32>(rect.height)) / 2;
  } else {
    u32 safe_area = 100;
    if (allow_overscan_cutoff && safe_area_x > 0 && safe_area_x < 100) {
      safe_area = safe_area_x;
    }
    rect.width = RescaleUnsigned(surface_height, aspect_x, aspect_y);
    bool bars = false;
    if (rect.width * safe_area > surface_width * 100) {
      rect.width = RescaleUnsigned(surface_width, 100, safe_area);
      bars = true;
    }
    if (bars && letterbox) {
      rect.height = RescaleUnsigned(surface_width * 100, aspect_y, aspect_x * safe_area);
      rect.y = (static_cast<i32>(surface_height) - static_cast<i32>(rect.height)) / 2;
    } else {
      rect.height = surface_height;
    }
    rect.x = (static_cast<i32>(surface_width) - static_cast<i32>(rect.width)) / 2;
  }
  return rect;
}

/**
 * Returns how long an overlay only paint waits so overlay paints keep kOverlayPaintInterval apart.
 * It is zero when the interval has passed.
 *
 * @param last_paint The time the previous paint started.
 */
constexpr std::chrono::nanoseconds GetOverlayPaintDelay(
    std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point last_paint) {
  const auto elapsed = now - last_paint;
  return elapsed >= kOverlayPaintInterval ? std::chrono::nanoseconds{0}
                                          : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                kOverlayPaintInterval - elapsed);
}

/**
 * Holds back overlay only paints so they keep an interval apart, and lets a new guest frame cut the
 * wait short.
 */
class OverlayPaintPacer {
 public:
  /** @param interval The least time between the starts of two overlay only paints. */
  explicit OverlayPaintPacer(std::chrono::nanoseconds interval = kOverlayPaintInterval)
      : interval_(interval) {}

  /** Marks a new guest frame as ready and wakes a waiting paint. Any thread. */
  void SignalGuestOutput() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      guest_output_pending_ = true;
    }
    condition_.notify_all();
  }

  /**
   * Waits until an overlay only paint is allowed, unless a guest frame is ready or the paint is
   * forced, and consumes the ready flag.
   *
   * @param last_paint The time the previous paint started.
   * @param force True to skip the wait.
   * @return True when a guest frame was ready.
   */
  bool Wait(std::chrono::steady_clock::time_point last_paint, bool force) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!force && !guest_output_pending_) {
      const auto elapsed = std::chrono::steady_clock::now() - last_paint;
      if (elapsed < interval_) {
        condition_.wait_for(lock, interval_ - elapsed, [this] { return guest_output_pending_; });
      }
    }
    const bool pending = guest_output_pending_;
    guest_output_pending_ = false;
    return pending;
  }

 private:
  std::chrono::nanoseconds interval_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool guest_output_pending_ = false;
};

}  // namespace rex::gpu
