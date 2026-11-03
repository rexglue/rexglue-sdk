/**
 * @file        tests/pull_device.h
 * @brief       Fake audio device thread that pulls samples
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <rex/apu/audio_driver.h>

namespace rex::test {

class PullDevice {
 public:
  using Sink = std::function<void(std::span<const f32>)>;

  PullDevice() = default;
  PullDevice(const PullDevice&) = delete;
  PullDevice& operator=(const PullDevice&) = delete;
  ~PullDevice() { Close(); }

  void Open(rex::apu::AudioFill fill, u32 channels, Sink sink) {
    Close();
    stop_ = false;
    thread_ =
        std::thread([this, fill = std::move(fill), channels, sink = std::move(sink)]() mutable {
          std::vector<f32> buffer;
          while (!stop_) {
            const u32 frames = request_;
            if (!paused_) {
              buffer.resize(static_cast<size_t>(frames) * channels);
              fill(std::span<f32>(buffer), channels);
              if (sink) {
                sink(buffer);
              }
            }
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(1'000'000'000LL * frames / rex::apu::kAudioSampleRate));
          }
        });
  }

  void Close() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /** Sets the sample frames asked for on every pull. */
  void SetRequest(u32 frames) { request_ = frames; }

  /** Pauses or resumes the pulls. A pull under way finishes. */
  void SetPaused(bool paused) { paused_ = paused; }

 private:
  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<u32> request_{rex::apu::kAudioChannelSamples};
  std::thread thread_;
};

}  // namespace rex::test
