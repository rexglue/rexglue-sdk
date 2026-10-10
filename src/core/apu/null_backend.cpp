/**
 * @file        core/apu/null_backend.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/apu/backend.h>

#include <chrono>

#include <rex/platform/thread.h>
#include <rex/move_only_function.h>

namespace rex::apu {

NullBackend::NullBackend() {
  clock_ = platform::Spawn("APU Null Clock", [this](const std::stop_token& stop) {
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<f64>(static_cast<f64>(kChannelSamples) / kFrameRate));
    auto next = Clock::now() + period;
    while (!stop.stop_requested()) {
      std::this_thread::sleep_until(next);
      next += period;
      Tick();
    }
  });
}

NullBackend::~NullBackend() {
  clock_ = {};
}

Result<u32> NullBackend::OpenStream(rex::MoveOnlyFunction<void()> on_frame_played) {
  std::lock_guard lock(mutex_);
  const u32 id = next_stream_++;
  streams_.emplace(id, Stream{std::move(on_frame_played), 0});
  return id;
}

void NullBackend::CloseStream(u32 stream) {
  std::lock_guard lock(mutex_);
  streams_.erase(stream);
}

void NullBackend::Submit(u32 stream, Frame) {
  std::lock_guard lock(mutex_);
  if (auto it = streams_.find(stream); it != streams_.end()) {
    ++it->second.queued;
  }
}

void NullBackend::Tick() {
  std::lock_guard lock(mutex_);
  for (auto& [id, stream] : streams_) {
    if (stream.queued == 0) {
      continue;
    }
    --stream.queued;
    stream.on_frame_played();
  }
}

void NullBackend::SetStereoFold(const StereoFold& fold) {
  std::lock_guard lock(mutex_);
  fold_ = fold;
}

StereoFold NullBackend::stereo_fold() const {
  std::lock_guard lock(mutex_);
  return fold_;
}

void NullBackend::SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard lock(mutex_);
  mix_ = mix;
}

SurroundMix NullBackend::surround_mix() const {
  std::lock_guard lock(mutex_);
  return mix_;
}

void NullBackend::SetOutputGain(f32 linear) {
  std::lock_guard lock(mutex_);
  gain_ = linear;
}

f32 NullBackend::output_gain() const {
  std::lock_guard lock(mutex_);
  return gain_;
}

}  // namespace rex::apu
