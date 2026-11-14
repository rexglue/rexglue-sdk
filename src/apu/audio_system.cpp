#include "apu/audio_system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/apu/audio_driver.h>
#include <rex/logging/log_category.h>
#include <rex/utility.h>

#include "apu/mix.h"

namespace rex::apu {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPeriod = std::chrono::nanoseconds(1'000'000'000LL * rex::apu::kAudioChannelSamples /
                                                  rex::apu::kAudioSampleRate);
constexpr u32 kReopenPeriods = 188;
constexpr auto kDeviceWatch = std::chrono::milliseconds(20);

struct QueuedFrame {
  std::array<f32, rex::apu::kAudioFrameSamples> samples;
  Clock::time_point submitted;
};

struct Slot {
  bool open = false;
  u32 generation = 0;
  std::deque<QueuedFrame> queue;
  MoveOnlyFunction<void()> on_frame_played;
};

struct MixSlot {
  u32 generation = 0;
  StreamGuard guard;
};

}  // namespace

struct AudioSystem::State {
  rex::apu::IAudioDriver* driver = nullptr;
  std::atomic<bool> focused{true};

  mutable std::mutex mutex;
  std::array<Slot, kMaxStreams> slots;
  u32 next_generation = 1;

  std::thread thread;
  std::mutex wake_mutex;
  std::condition_variable wake;
  bool stop = false;

  std::atomic<u32> channels{2};
  std::atomic<u64> underruns{0};
  std::atomic<u32> latency_us{0};

  std::mutex mix_mutex;
  std::array<f32, rex::apu::kAudioFrameSamples> planar{};
  std::array<f32, rex::apu::kAudioFrameSamples> out{};
  u32 out_samples = 0;
  u32 out_pos = 0;
  u32 out_channels = 0;
  bool pad = false;
  MoveOnlyFunction<void(u32)> mix_period;

  std::array<MixSlot, kMaxStreams> mix_slots;
  f32 master_applied = 1.0f;
  bool device_open = false;
  bool reopen_failed = false;
  u32 reopen_wait = 0;
  u64 latency_sum_us = 0;
  u64 latency_count = 0;
  Clock::time_point latency_window = Clock::now();
};

rex::apu::AudioSettings::AudioSettings() : SettingOwner("apu") {}

AudioSystem::AudioSystem() : state_(std::make_unique<State>()) {}

AudioSystem::~AudioSystem() {
  Stop();
}

void AudioSystem::SetFocused(bool focused) {
  state_->focused = focused;
}

Result<rex::apu::AudioStreamID> AudioSystem::OpenStream(MoveOnlyFunction<void()> on_frame_played) {
  std::lock_guard lock(state_->mutex);
  for (u32 i = 0; i < kMaxStreams; ++i) {
    Slot& slot = state_->slots[i];
    if (slot.open) {
      continue;
    }
    slot.open = true;
    slot.generation = state_->next_generation++;
    slot.queue.clear();
    slot.on_frame_played = std::move(on_frame_played);
    return rex::apu::AudioStreamID(i + 1);
  }
  return Err<rex::apu::AudioStreamID>(ErrorCategory::kRuntime, "all audio streams are open");
}

void AudioSystem::CloseStream(rex::apu::AudioStreamID stream) {
  const u32 index = static_cast<u32>(stream);
  if (index == 0 || index > kMaxStreams) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  Slot& slot = state_->slots[index - 1];
  slot.open = false;
  slot.queue.clear();
  slot.on_frame_played = nullptr;
}

Result<void> AudioSystem::Submit(rex::apu::AudioStreamID stream, rex::apu::AudioFrame frame) {
  const u32 index = static_cast<u32>(stream);
  if (index == 0 || index > kMaxStreams) {
    return Err(ErrorCategory::kNotFound, "unknown audio stream");
  }
  std::lock_guard lock(state_->mutex);
  Slot& slot = state_->slots[index - 1];
  if (!slot.open) {
    return Err(ErrorCategory::kNotFound, "unknown audio stream");
  }
  if (slot.queue.size() >= settings_.max_queued_frames.Get()) {
    return Err(ErrorCategory::kRuntime, "audio stream queue is full");
  }
  QueuedFrame queued;
  std::ranges::copy(frame, queued.samples.begin());
  queued.submitted = Clock::now();
  slot.queue.push_back(queued);
  return Ok();
}

u32 AudioSystem::GetOutputChannels() const {
  return state_->channels.load();
}

rex::apu::AudioStats AudioSystem::GetStats() const {
  rex::apu::AudioStats stats;
  {
    std::lock_guard lock(state_->mutex);
    for (const Slot& slot : state_->slots) {
      if (!slot.open) {
        continue;
      }
      ++stats.open_streams;
      stats.queued_frames += static_cast<u32>(slot.queue.size());
    }
  }
  stats.underruns = state_->underruns.load();
  stats.latency_us = state_->latency_us.load();
  return stats;
}

Result<void> AudioSystem::Start(rex::apu::IAudioDriver* device) {
  State& s = *state_;
  if (s.thread.joinable()) {
    return Err(ErrorCategory::kRuntime, "audio system already started");
  }
  s.driver = device;
  s.stop = false;
  s.device_open = false;
  s.reopen_wait = 0;
  s.reopen_failed = false;
  s.channels = 2;
  s.out_samples = 0;
  s.out_pos = 0;
  s.out_channels = 0;

  s.mix_period = [this](u32 out_channels) {
    State& s = *state_;
    std::array<bool, kMaxStreams> have{};
    std::array<QueuedFrame, kMaxStreams> taken;
    std::array<u32, kMaxStreams> generations{};
    {
      std::lock_guard lock(s.mutex);
      const auto now = Clock::now();
      for (u32 i = 0; i < kMaxStreams; ++i) {
        Slot& slot = s.slots[i];
        if (!slot.open) {
          continue;
        }
        generations[i] = slot.generation;
        if (slot.queue.empty()) {
          s.underruns.fetch_add(1);
          s.pad = true;
          continue;
        }
        taken[i] = slot.queue.front();
        slot.queue.pop_front();
        have[i] = true;
        s.latency_sum_us += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - taken[i].submitted)
                .count());
        ++s.latency_count;
        if (slot.on_frame_played) {
          slot.on_frame_played();
        }
      }
      if (now - s.latency_window >= std::chrono::seconds(1)) {
        s.latency_us = s.latency_count ? static_cast<u32>(s.latency_sum_us / s.latency_count) : 0;
        s.latency_sum_us = 0;
        s.latency_count = 0;
        s.latency_window = now;
      }
    }

    const f64 limit = settings_.peak_limit.Get();
    s.planar.fill(0.0f);
    for (u32 i = 0; i < kMaxStreams; ++i) {
      MixSlot& slot = s.mix_slots[i];
      if (generations[i] == 0) {
        slot = MixSlot{};
        continue;
      }
      if (slot.generation != generations[i]) {
        slot = MixSlot{};
        slot.generation = generations[i];
      }
      if (!have[i]) {
        continue;
      }
      slot.guard.Apply(taken[i].samples, limit);
      for (u32 k = 0; k < rex::apu::kAudioFrameSamples; ++k) {
        s.planar[k] += taken[i].samples[k];
      }
    }

    const bool muted =
        settings_.mute.Get() || (settings_.mute_unfocused.Get() && !s.focused.load());
    const f32 master_target = muted ? 0.0f : static_cast<f32>(settings_.gain.Get());
    f32 constant_gain = master_target;
    if (s.master_applied != master_target) {
      RampGain(s.planar.data(), rex::apu::kAudioChannelSamples, s.master_applied, master_target);
      constant_gain = 1.0f;
    }
    s.master_applied = master_target;

    if (out_channels == rex::apu::kAudioFrameChannels) {
      const SurroundWeights weights{static_cast<f32>(settings_.center.Get()),
                                    static_cast<f32>(settings_.surround.Get()),
                                    static_cast<f32>(settings_.lfe.Get())};
      MixSurround(s.out.data(), s.planar.data(), rex::apu::kAudioChannelSamples, weights,
                  constant_gain);
    } else {
      MixStereo(s.out.data(), s.planar.data(), rex::apu::kAudioChannelSamples, StereoWeights{},
                constant_gain);
    }
    s.out_channels = out_channels;
    s.out_samples = out_channels * rex::apu::kAudioChannelSamples;
    s.out_pos = 0;
  };

  auto make_fill = [this] {
    return rex::apu::AudioFill([this](std::span<f32> samples, u32 channels) {
      State& s = *state_;
      std::lock_guard lock(s.mix_mutex);
      s.pad = false;
      size_t done = 0;
      while (done < samples.size()) {
        if (s.out_pos >= s.out_samples || s.out_channels != channels) {
          s.mix_period(channels);
        }
        const size_t count = std::min<size_t>(samples.size() - done, s.out_samples - s.out_pos);
        std::copy_n(s.out.data() + s.out_pos, count, samples.data() + done);
        done += count;
        s.out_pos += static_cast<u32>(count);
      }
    });
  };

  if (s.driver) {
    if (auto opened = s.driver->OpenDevice(rex::apu::kAudioFrameChannels, make_fill())) {
      s.channels = *opened;
      s.device_open = true;
    } else {
      REXAPU_WARN("the audio device did not open, {}", opened.error().message);
      s.reopen_failed = true;
    }
  }
  s.thread = std::thread([this, make_fill] {
    State& s = *state_;
    auto next = Clock::now() + kPeriod;
    for (;;) {
      const bool pulled = s.driver && s.device_open;
      {
        std::unique_lock lock(s.wake_mutex);
        if (s.wake.wait_until(lock, pulled ? Clock::now() + kDeviceWatch : next,
                              [&] { return s.stop; })) {
          break;
        }
      }
      if (pulled) {
        if (s.driver->IsDeviceLost()) {
          REXAPU_WARN("the audio device was lost");
          s.driver->CloseDevice();
          s.device_open = false;
          s.channels = 2;
          s.reopen_wait = kReopenPeriods;
          next = Clock::now() + kPeriod;
        }
      } else {
        next += kPeriod;
        if (next < Clock::now()) {
          next = Clock::now() + kPeriod;
        }
        {
          std::lock_guard lock(s.mix_mutex);
          s.mix_period(s.channels.load());
        }
        if (s.driver) {
          if (s.reopen_wait > 0) {
            --s.reopen_wait;
          } else if (auto opened =
                         s.driver->OpenDevice(rex::apu::kAudioFrameChannels, make_fill())) {
            s.channels = *opened;
            s.device_open = true;
            if (s.reopen_failed) {
              REXAPU_INFO("the audio device opened");
            }
            s.reopen_failed = false;
          } else {
            if (!s.reopen_failed) {
              REXAPU_WARN("the audio device did not reopen, {}", opened.error().message);
            } else {
              REXAPU_DEBUG("the audio device still did not reopen, {}", opened.error().message);
            }
            s.reopen_failed = true;
            s.reopen_wait = kReopenPeriods;
          }
        }
      }
    }
    if (s.driver && s.device_open) {
      s.driver->CloseDevice();
      s.device_open = false;
    }
  });
  return Ok();
}

void AudioSystem::Stop() {
  if (!state_->thread.joinable()) {
    return;
  }
  {
    std::lock_guard lock(state_->wake_mutex);
    state_->stop = true;
  }
  state_->wake.notify_all();
  state_->thread.join();
  state_->driver = nullptr;
  state_->channels = 2;
}

}  // namespace rex::apu
