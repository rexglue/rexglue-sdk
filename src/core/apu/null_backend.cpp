/**
 * @file        core/apu/null_backend.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/apu.h>

#include <chrono>

#include <rex/platform/thread.h>
#include <rex/move_only_function.h>

namespace rex::apu {

NullAPUBackend::NullAPUBackend() {
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

NullAPUBackend::~NullAPUBackend() {
  clock_ = {};
}

Result<u32> NullAPUBackend::OpenStream(rex::MoveOnlyFunction<void()> on_frame_played) {
  std::lock_guard lock(mutex_);
  const u32 id = next_stream_++;
  streams_.emplace(id, Stream{std::move(on_frame_played), 0});
  return id;
}

void NullAPUBackend::CloseStream(u32 stream) {
  std::lock_guard lock(mutex_);
  streams_.erase(stream);
}

void NullAPUBackend::Submit(u32 stream, Frame) {
  std::lock_guard lock(mutex_);
  if (auto it = streams_.find(stream); it != streams_.end()) {
    ++it->second.queued;
  }
}

void NullAPUBackend::Tick() {
  std::lock_guard lock(mutex_);
  for (auto& [id, stream] : streams_) {
    if (stream.queued == 0) {
      continue;
    }
    --stream.queued;
    stream.on_frame_played();
  }
}

void NullAPUBackend::SetStereoFold(const StereoFold& fold) {
  std::lock_guard lock(mutex_);
  fold_ = fold;
}

StereoFold NullAPUBackend::GetStereoFold() const {
  std::lock_guard lock(mutex_);
  return fold_;
}

void NullAPUBackend::SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard lock(mutex_);
  mix_ = mix;
}

SurroundMix NullAPUBackend::GetSurroundMix() const {
  std::lock_guard lock(mutex_);
  return mix_;
}

void NullAPUBackend::SetOutputGain(f32 linear) {
  std::lock_guard lock(mutex_);
  gain_ = linear;
}

f32 NullAPUBackend::GetOutputGain() const {
  std::lock_guard lock(mutex_);
  return gain_;
}

}  // namespace rex::apu
