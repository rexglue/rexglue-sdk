#include "apu/sdl3/sdl3_backend.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/move_only_function.h>

#include "apu/sdl3/conversion.h"
#include "apu/sdl3/cvar_table.h"

namespace rex::apu::sdl3 {

SDL3Backend::SDL3Backend() = default;

SDL3Backend::~SDL3Backend() {
  Shutdown();
}

Result<void> SDL3Backend::Init() {
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return Err(ErrorCategory::Platform,
               std::format("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError()));
  }
  initialized_ = true;
  return Ok();
}

void SDL3Backend::Shutdown() {
  std::map<u32, std::unique_ptr<Stream>> streams;
  {
    std::lock_guard lock(streams_mutex_);
    streams.swap(streams_);
  }
  for (auto& [id, stream] : streams) {
    SDL_DestroyAudioStream(stream->sdl);
  }
  streams.clear();
  if (initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    initialized_ = false;
  }
}

Result<u32> SDL3Backend::OpenStream(rex::MoveOnlyFunction<void()> on_frame_played) {
  auto stream = std::make_unique<Stream>();
  stream->owner = this;
  stream->on_frame_played = std::move(on_frame_played);
  REX_TRY(stream->sdl, OpenDeviceStream(*stream));
  std::lock_guard lock(streams_mutex_);
  const u32 id = next_stream_++;
  streams_.emplace(id, std::move(stream));
  return id;
}

void SDL3Backend::CloseStream(u32 stream) {
  std::unique_ptr<Stream> closing;
  {
    std::lock_guard lock(streams_mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) {
      return;
    }
    closing = std::move(it->second);
    streams_.erase(it);
  }
  SDL_DestroyAudioStream(closing->sdl);
}

void SDL3Backend::Submit(u32 stream, Frame frame) {
  REX_PROFILE_SCOPE("apu");
  Stream* target = nullptr;
  {
    std::lock_guard lock(streams_mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) {
      return;
    }
    target = it->second.get();
  }
  std::unique_ptr<f32[]> buffer;
  {
    std::lock_guard lock(target->frames_mutex);
    if (!target->unused.empty()) {
      buffer = std::move(target->unused.top());
      target->unused.pop();
    }
  }
  if (!buffer) {
    buffer = std::make_unique<f32[]>(kFrameSamples);
  }
  std::memcpy(buffer.get(), frame.data(), kFrameSamples * sizeof(f32));
  std::lock_guard lock(target->frames_mutex);
  target->queued.push(std::move(buffer));
  REX_PERF_SET(kBufferQueueDepth, static_cast<i64>(target->queued.size()));
}

Result<SDL_AudioStream*> SDL3Backend::OpenDeviceStream(Stream& stream) {
  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = static_cast<int>(kFrameRate);
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = static_cast<int>(kFrameChannels);
  stream.device_channels = kFrameChannels;
  SDL_AudioStream* sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                                   Callback, &stream);
  if (!sdl) {
    return Err<SDL_AudioStream*>(
        ErrorCategory::Platform,
        std::format("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError()));
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl);
  if (!sdl_device) {
    SDL_DestroyAudioStream(sdl);
    return Err<SDL_AudioStream*>(
        ErrorCategory::Platform,
        std::format("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError()));
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, nullptr)) {
    REXLOG_WARN(apu, "SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels <= 2) {
    SDL_DestroyAudioStream(sdl);
    desired_spec.channels = 2;
    stream.device_channels = 2;
    sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec, Callback,
                                    &stream);
    if (!sdl) {
      return Err<SDL_AudioStream*>(
          ErrorCategory::Platform,
          std::format("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError()));
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl);
    if (!sdl_device) {
      SDL_DestroyAudioStream(sdl);
      return Err<SDL_AudioStream*>(
          ErrorCategory::Platform,
          std::format("SDL_GetAudioStreamDevice() failed after stereo fallback: {}",
                      SDL_GetError()));
    }
  }

  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXLOG_INFO(apu, "audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<u32>(obtained_spec.format), stream.device_channels);

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    SDL_DestroyAudioStream(sdl);
    return Err<SDL_AudioStream*>(ErrorCategory::Platform,
                                 std::format("SDL_ResumeAudioDevice() failed: {}", SDL_GetError()));
  }
  return sdl;
}

void SDL3Backend::Callback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                           [[maybe_unused]] int total_amount) {
  REX_PROFILE_SCOPE("apu");
  if (!userdata || !stream) {
    REXLOG_ERROR(apu, "SDL3Backend::Callback called with nullptr.");
    return;
  }
  auto* stream_state = static_cast<Stream*>(userdata);
  const int sample_count =
      static_cast<int>(kChannelSamples * std::max<u32>(stream_state->device_channels, 1));
  const int len = static_cast<int>(sizeof(f32) * sample_count);
  f32* data = SDL_stack_alloc(f32, sample_count);
  if (!data) {
    REXLOG_ERROR(apu, "SDL3Backend::Callback failed to allocate {} samples", sample_count);
    return;
  }
  const StereoFold fold = stream_state->owner->stereo_fold();
  const SurroundMix mix = stream_state->owner->surround_mix();
  const f32 gain = stream_state->owner->output_gain();
  while (additional_amount > 0) {
    std::unique_lock<std::mutex> guard(stream_state->frames_mutex);
    if (stream_state->queued.empty()) {
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXLOG_ERROR(apu, "SDL_PutAudioStreamData() failed while filling silence: {}",
                     SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      std::unique_ptr<f32[]> buffer = std::move(stream_state->queued.front());
      stream_state->queued.pop();
      if (REXCVAR_GET(cvars::mute)) {
        std::memset(data, 0, len);
      } else {
        switch (stream_state->device_channels) {
          case 2:
            sequential_6_BE_to_interleaved_2_LE(data, buffer.get(), kChannelSamples, fold, gain);
            break;
          case 6:
            sequential_6_BE_to_interleaved_6_LE(data, buffer.get(), kChannelSamples, mix, gain);
            break;
          default:
            REX_UNHANDLED_CASE(stream_state->device_channels);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXLOG_ERROR(apu, "SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        stream_state->unused.push(std::move(buffer));
        break;
      }
      stream_state->unused.push(std::move(buffer));
      stream_state->on_frame_played();
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

void SDL3Backend::SetStereoFold(const StereoFold& fold) {
  std::lock_guard lock(mix_mutex_);
  fold_ = fold;
}

StereoFold SDL3Backend::stereo_fold() const {
  std::lock_guard lock(mix_mutex_);
  return fold_;
}

void SDL3Backend::SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard lock(mix_mutex_);
  mix_ = mix;
}

SurroundMix SDL3Backend::surround_mix() const {
  std::lock_guard lock(mix_mutex_);
  return mix_;
}

void SDL3Backend::SetOutputGain(f32 linear) {
  std::lock_guard lock(mix_mutex_);
  gain_ = linear;
}

f32 SDL3Backend::output_gain() const {
  std::lock_guard lock(mix_mutex_);
  return gain_;
}

}  // namespace rex::apu::sdl3
