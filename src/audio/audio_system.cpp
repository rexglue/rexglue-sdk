/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_registry.h>
#include <rex/system/thread_state.h>
#include <rex/platform/thread.h>
#include <rex/cvar.h>

#include "audio/cvar_table.h"

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace rex::audio {

namespace {
constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
}  // namespace

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ =
      std::min(static_cast<uint32_t>(kMaximumQueuedFrames),
               std::max(static_cast<uint32_t>(REXCVAR_GET(::rex::audio::cvars::max_queued_frames)),
                        static_cast<uint32_t>(4)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::platform::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::platform::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::platform::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  kernel_state_ = kernel_state;
  auto record = std::make_unique<system::ThreadRecord>(kernel_state, 128 * 1024, 0, [this]() {
    WorkerThreadMain();
    return 0;
  });
  record->set_name("Audio Worker");
  kernel_state->threads()->Launch(std::move(record), &worker_thread_);

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result = rex::platform::WaitAny(wait_handles_, rex::countof(wait_handles_), true,
                                         std::chrono::milliseconds(500));
    if (result.first == rex::platform::WaitResult::kFailed) {
      REXLOG_WARN(audio, "AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::platform::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXLOG_DEV(audio, "AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == platform::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        platform::Wait(resume_event_.get(), false);
      }

      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::platform::WaitResult::kSuccess) {
      auto index = result.second;

      uint32_t client_callback;
      uint32_t client_callback_arg;
      {
        std::lock_guard lock(mutex_);
        client_callback = clients_[index].callback;
        client_callback_arg = clients_[index].wrapped_callback_arg;
      }

      if (client_callback) {
        if (diag_pump_count < 10) {
          REXLOG_DEBUG(audio,
                       "AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        REX_PROFILE_SCOPE_TEXT("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        function_dispatcher_->Execute(system::ThreadRecord::GetCurrentThread()->thread_state(),
                                      client_callback, args, rex::countof(args));
        if (diag_pump_count < 10) {
          REXLOG_DEBUG(audio, "AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
      } else {
        REXLOG_DEBUG(audio, "AudioWorker: semaphore signaled for client {} but callback is 0",
                     index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      REX_PROFILE_SCOPE_TEXT("apu", "Sleep");
      rex::platform::Sleep(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked on
    // guest objects (e.g. KeWaitForMultipleObjects), so terminating is the last
    // resort. Give it a chance to unwind first: TerminateThread abandons any
    // lock the thread holds, including the CRT heap lock.
    {
      system::ThreadPin pin = kernel_state_->threads()->FindByHandle(worker_thread_);
      rex::platform::Thread* host_thread = pin ? pin->thread() : nullptr;
      bool exited =
          host_thread && rex::platform::Wait(host_thread, false, kWorkerShutdownTimeout) ==
                             rex::platform::WaitResult::kSuccess;
      if (!exited) {
        REXLOG_WARN(audio, "Audio worker did not exit within {} ms; terminating",
                    kWorkerShutdownTimeout.count());
        if (pin) {
          pin->Terminate(0);
        }
      }
    }
    kernel_state_->object_table()->Close(worker_thread_);
    worker_thread_ = 0;
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXLOG_DEBUG(audio, "AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  std::lock_guard lock(mutex_);

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXLOG_DEBUG(audio, "AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  REX_PROFILE_SCOPE("apu");

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXLOG_DEBUG(audio, "AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  std::lock_guard lock(mutex_);
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  REX_PROFILE_SCOPE("apu");

  std::lock_guard lock(mutex_);
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::platform::WaitResult wait_result;
  do {
    wait_result = rex::platform::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::platform::WaitResult::kSuccess);
  assert_true(wait_result == rex::platform::WaitResult::kTimeout);
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
