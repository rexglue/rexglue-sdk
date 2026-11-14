/**
 * @file        gpu/xenos/host/present_policy.h
 * @brief       Present timing decisions that need no GPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

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
 * @return FIFO for vsync, otherwise the first allowed of immediate, mailbox and relaxed FIFO, and
 * FIFO when none is allowed.
 */
constexpr VulkanPresentMode ChooseVulkanPresentMode(bool vsync,
                                                    const VulkanPresentModeSupport& support) {
  if (vsync) {
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
