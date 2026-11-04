#include "kernel/apu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

#include <rex/platform/perf.h>
#include <rex/logging/log_category.h>
#include <rex/apu/audio_driver.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>

#include "kernel/thread_wait.h"
#include "kernel/xma/decoder.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
constexpr u32 kDefaultQueuedFrames = 8;

}  // namespace

APU::APU(TitleRuntime& runtime, rex::apu::IAudioSystem* audio)
    : runtime_(runtime),
      audio_(audio),
      queued_frames_(audio ? audio->GetSettings().max_queued_frames.Get() : kDefaultQueuedFrames) {
  for (u32 i = 0; i < kMaxClients; ++i) {
    semaphores_[i] = platform::Semaphore::Create(0, static_cast<i32>(queued_frames_));
    wait_handles_[i] = semaphores_[i].get();
  }
  shutdown_event_ = platform::Event::CreateAutoResetEvent(false);
  wait_handles_[kMaxClients] = shutdown_event_.get();
  xma_decoder_ = std::make_unique<XMADecoder>(StateOf(runtime).GetFunctionDispatcher());
  resume_event_ = platform::Event::CreateAutoResetEvent(false);
}

APU::~APU() {
  Shutdown();
}

Result<void> APU::Setup() {
  if (X_STATUS status = xma_decoder_->Setup(); status != X_STATUS_SUCCESS) {
    return Err(StatusError(status));
  }
  worker_running_ = true;
  auto thread = std::make_unique<KernelThread>(runtime_, 128 * 1024, 0, [this]() {
    WorkerMain();
    return 0;
  });
  thread->SetName("Audio Worker");
  auto launched = runtime_.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    worker_running_ = false;
    return std::unexpected(launched.error());
  }
  worker_thread_ = *launched;
  return Ok();
}

void APU::WorkerMain() {
  while (worker_running_) {
    auto result = platform::WaitAny(wait_handles_.data(), wait_handles_.size(), true,
                                    std::chrono::milliseconds(500));
    if (result.first == platform::WaitResult::kFailed) {
      REXAPU_WARN("Audio worker: WaitAny failed");
      continue;
    }

    if (result.first == platform::WaitResult::kSuccess && result.second == kMaxClients) {
      if (paused_) {
        pause_fence_.Signal();
        platform::Wait(resume_event_.get(), false);
      }
      continue;
    }

    bool pumped = false;
    if (result.first == platform::WaitResult::kSuccess) {
      const auto index = result.second;
      u32 client_callback;
      u32 client_callback_arg;
      {
        std::lock_guard lock(mutex_);
        client_callback = clients_[index].callback;
        client_callback_arg = clients_[index].wrapped_callback_arg;
      }
      if (client_callback) {
        REX_PROFILE_SCOPE_TEXT("apu", "rex::kernel::APU->client_callback");
        u64 args[] = {client_callback_arg};
        StateOf(runtime_).GetFunctionDispatcher()->Execute(
            KernelThread::GetCallingThread()->GetThreadState(), client_callback, args,
            std::size(args));
      }
      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      REX_PROFILE_SCOPE_TEXT("apu", "Sleep");
      platform::SleepFor(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;
}

void APU::StopWorker() {
  {
    std::lock_guard pause_lock(pause_mutex_);
    if (worker_running_) {
      xma_decoder_->Shutdown();
      worker_running_ = false;
      shutdown_event_->Set();
    }
    paused_ = false;
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
  }
  if (worker_thread_) {
    {
      if (ThreadPin pin = runtime_.GetThreadRegistry().FindByHandle(worker_thread_)) {
        pin->RequestStop();
        std::array<ThreadPin, 1> threads{std::move(pin)};
        WaitForThreadsToStop(threads, kWorkerShutdownTimeout, [](const ThreadPin&) {
          REXAPU_WARN("Audio worker still running after {} ms", kWorkerShutdownTimeout.count());
        });
      }
    }
    CloseOwned(runtime_.GetObjectTable(), worker_thread_);
    worker_thread_ = 0;
  }
}

void APU::Shutdown() {
  StopWorker();
  std::lock_guard lock(mutex_);
  for (Client& client : clients_) {
    if (!client.in_use) {
      continue;
    }
    audio_->CloseStream(client.stream);
    if (client.wrapped_callback_arg) {
      StateOf(runtime_).GetAddressSpace()->SystemHeapFree(client.wrapped_callback_arg);
    }
    client = {};
  }
}

Result<u32> APU::RegisterClient(u32 callback, u32 callback_arg) {
  if (!audio_) {
    REXAPU_ERROR("the program has no audio system, a render client is refused");
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  std::lock_guard lock(mutex_);
  auto free = std::ranges::find_if(clients_, [](const Client& c) { return !c.in_use; });
  if (free == clients_.end()) {
    return Err<u32>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 index = static_cast<u32>(free - clients_.begin());
  platform::Semaphore* semaphore = semaphores_[index].get();
  auto stream = audio_->OpenStream([semaphore] { semaphore->Release(1, nullptr); });
  if (!stream) {
    REXAPU_ERROR("the audio system refused a stream, {}", stream.error().message);
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  semaphore->Release(static_cast<i32>(queued_frames_), nullptr);
  const u32 ptr = StateOf(runtime_).GetAddressSpace()->SystemHeapAlloc(0x4);
  *StateOf(runtime_).GetAddressSpace()->TranslateVirtual<be_u32*>(ptr) = callback_arg;
  *free = {*stream, callback, callback_arg, ptr, true};
  return index;
}

void APU::UnregisterClient(u32 index) {
  REX_PROFILE_SCOPE("apu");
  std::lock_guard lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  audio_->CloseStream(clients_[index].stream);
  StateOf(runtime_).GetAddressSpace()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {};
  platform::Semaphore* semaphore = semaphores_[index].get();
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {}
}

void APU::SubmitFrame(u32 index, u32 samples_ptr) {
  REX_PROFILE_SCOPE("apu");
  std::lock_guard lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  const auto* samples =
      StateOf(runtime_).GetAddressSpace()->TranslateVirtual<const be_f32*>(samples_ptr);
  std::array<f32, rex::apu::kAudioFrameSamples> frame;
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = samples[i];
  }
  if (auto submitted = audio_->Submit(clients_[index].stream, frame);
      !submitted && !clients_[index].refused_logged) {
    clients_[index].refused_logged = true;
    REXAPU_WARN("the audio system refused a frame for client {}, {}", index,
                submitted.error().message);
  }
}

u32 APU::GetOutputChannels() const {
  return audio_ ? audio_->GetOutputChannels() : 2;
}

void APU::SetPaused(bool paused) {
  std::lock_guard pause_lock(pause_mutex_);
  if (paused_ == paused) {
    return;
  }
  paused_ = paused;
  if (paused) {
    if (worker_running_) {
      shutdown_event_->Set();
      pause_fence_.Wait();
      parked_ = true;
    }
    xma_decoder_->Pause();
  } else {
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
    xma_decoder_->Resume();
  }
}

u32 APU::CountCredits(u32 index) {
  platform::Semaphore* semaphore = semaphores_[index].get();
  u32 count = 0;
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {
    ++count;
  }
  if (count > 0) {
    semaphore->Release(static_cast<i32>(count), nullptr);
  }
  return count;
}

bool APU::TakeCredit(u32 index) {
  return platform::Wait(semaphores_[index].get(), false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess;
}

}  // namespace rex::kernel
