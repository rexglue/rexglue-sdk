/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/output_device.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_STRING(audio_device, "", "Audio",
                      "Sound output device, by the name rex::audio::ListOutputDevices gives; empty "
                      "follows the system's default device");

namespace rex::audio {
namespace {

// The device the sound plays on now, for CurrentOutputDevice.
std::mutex g_device_mutex;
std::string g_current_device;
// Set by audio_device's change callback, so the audio thread acts on a new
// choice at its next frame instead of at its next check.
std::atomic<bool> g_device_setting_changed{false};

struct PlaybackDevice {
  SDL_AudioDeviceID id;
  std::string base;  // the name the system gives it
  std::string name;  // the same, with " #2", " #3" ... after a name already listed
};

// The playback devices SDL knows now, in its order. SDL's names are the
// system's descriptions (PipeWire/PulseAudio: the sink's description;
// Windows: the endpoint's friendly name), not unique ids, so a second device
// with the same name gets a number.
std::vector<PlaybackDevice> PlaybackDevices() {
  std::vector<PlaybackDevice> list;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) return list;
  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
  if (!ids) return list;
  list.reserve(static_cast<size_t>(std::max(count, 0)));
  for (int i = 0; i < count; ++i) {
    const char* raw = SDL_GetAudioDeviceName(ids[i]);
    PlaybackDevice device{ids[i], raw && *raw ? raw : "Unnamed device", {}};
    int same = 1;
    for (const auto& other : list) same += other.base == device.base ? 1 : 0;
    device.name = same == 1 ? device.base : device.base + " #" + std::to_string(same);
    list.push_back(std::move(device));
  }
  SDL_free(ids);
  return list;
}

// The device named `wanted`, or 0 (the system default) when the name is empty
// or no such device is connected now.
SDL_AudioDeviceID FindPlaybackDevice(const std::string& wanted) {
  if (wanted.empty()) return 0;
  for (const auto& device : PlaybackDevices()) {
    if (device.name == wanted) return device.id;
  }
  return 0;
}

// The setting's value (read under the settings' own lock, so any thread may
// change it meanwhile).
std::string WantedDevice() { return rex::cvar::GetFlagByName("audio_device"); }

}  // namespace

std::vector<std::string> ListOutputDevices() {
  std::vector<std::string> names;
  for (auto& device : PlaybackDevices()) names.push_back(std::move(device.name));
  return names;
}

std::string CurrentOutputDevice() {
  std::lock_guard lock(g_device_mutex);
  return g_current_device;
}

}  // namespace rex::audio

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification. A title's SDL_HINT_APP_NAME
  // (set before this) wins over it: SDL reads the hint first.
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  // A new audio_device choice is picked up at the audio thread's next frame
  // (the callback runs under the settings' lock, so it only sets a flag);
  // the 2 s check catches changes made without a callback (a reset).
  static std::atomic<bool> callback_registered{false};
  if (!callback_registered.exchange(true)) {
    rex::cvar::RegisterChangeCallback("audio_device", [](std::string_view, std::string_view) {
      g_device_setting_changed = true;
    });
  }
  g_device_setting_changed = false;

  std::string outputs;
  for (const auto& device : PlaybackDevices()) {
    outputs += outputs.empty() ? "" : " | ";
    outputs += device.name;
  }
  REXAPU_INFO("audio outputs: {}", outputs.empty() ? "(none listed)" : outputs);

  const std::string wanted = WantedDevice();
  const SDL_AudioDeviceID device = FindPlaybackDevice(wanted);
  if (!wanted.empty() && !device) {
    REXAPU_WARN("audio_device '{}' isn't connected; using the system's default output", wanted);
  }
  std::lock_guard lock(stream_mutex_);
  opened_on_ = device;
  if (OpenStream(device)) return true;
  return device && OpenStream(0);
}

// Opens the output stream on `device` (0: the system default, which SDL
// follows when the default changes) and starts it. Called with stream_mutex_
// held and no stream open; the caller sets opened_on_ to the device it
// decided on, so a device that fails to open is tried again only when the
// setting changes or the device is plugged in again (a new id).
bool SDLAudioDriver::OpenStream(SDL_AudioDeviceID device) {
  const SDL_AudioDeviceID target = device ? device : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(target, &desired_spec, SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    CloseStream();
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  if (obtained_spec.channels <= 2) {
    CloseStream();
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(target, &desired_spec, SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      CloseStream();
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}' ({}): {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", device ? "chosen in audio_device" : "system default",
              obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    CloseStream();
    return false;
  }
  have_stream_ = true;
  UpdateCurrentDevice();
  return true;
}

void SDLAudioDriver::CloseStream() {
  have_stream_ = false;
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
}

void SDLAudioDriver::UpdateCurrentDevice() {
  const char* name = sdl_stream_ ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(sdl_stream_))
                                 : nullptr;
  std::lock_guard lock(g_device_mutex);
  g_current_device = name ? name : "";
}

// Runs on the audio thread for every frame, so it is a clock read unless the
// setting changed or its time comes (every 2 s: a device chosen being
// unplugged and plugged back in, or the setting reset; SDL itself follows the
// system default).
void SDLAudioDriver::CheckDevice() {
  const bool changed = g_device_setting_changed.exchange(false);
  const uint64_t now = SDL_GetTicks();
  if (!changed && now < next_device_check_) return;
  next_device_check_ = now + kDeviceCheckIntervalMs;

  std::lock_guard lock(stream_mutex_);
  const std::string wanted = WantedDevice();
  if (!changed && wanted.empty() && opened_on_ == 0) {
    UpdateCurrentDevice();  // the default may have moved
    return;
  }
  const SDL_AudioDeviceID device = FindPlaybackDevice(wanted);
  // The device decided on last time: nothing to do (also when it failed to
  // open, so a broken device or a missing sound server isn't retried and
  // logged every 2 s; a changed setting tries again).
  if (device == opened_on_ && (sdl_stream_ || !changed)) {
    UpdateCurrentDevice();
    return;
  }
  if (!wanted.empty() && !device) {
    REXAPU_WARN("audio_device '{}' isn't connected; using the system's default output", wanted);
  }
  opened_on_ = device;
  CloseStream();
  if (!OpenStream(device) && device) OpenStream(0);
  if (!sdl_stream_) UpdateCurrentDevice();
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  CheckDevice();
  if (!have_stream_) {
    // No output at all (every open failed): drop the frame at the rate it
    // would have played, so the guest's audio keeps its pace instead of
    // waiting forever for a device callback.
    std::this_thread::sleep_for(std::chrono::microseconds(1000000ull * channel_samples_ /
                                                          frame_frequency_));
    auto ret = semaphore_->Release(1, nullptr);
    assert_true(ret);
    return;
  }

  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  {
    std::lock_guard lock(stream_mutex_);
    CloseStream();
  }
  {
    std::lock_guard lock(g_device_mutex);
    g_current_device.clear();
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
