/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <chrono>
#include <queue>

#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::audio {

constexpr memory::fourcc_t kAudioSaveSignature = memory::make_fourcc("XAUD");

class AudioDriver;
class XmaDecoder;

class AudioSystem : public system::IAudioSystem {
 public:
  virtual ~AudioSystem();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  XmaDecoder* xma_decoder() const { return xma_decoder_.get(); }

  virtual X_STATUS Setup(system::KernelState* kernel_state);
  virtual void Shutdown();

  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index);
  void UnregisterClient(size_t index);
  void SubmitFrame(size_t index, uint32_t samples_ptr);

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  explicit AudioSystem(runtime::FunctionDispatcher* function_dispatcher);

  virtual void Initialize();

  void WorkerThreadMain();

  virtual X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                AudioDriver** out_driver) = 0;
  virtual void DestroyDriver(AudioDriver* driver) = 0;

  static constexpr size_t kMaximumQueuedFrames = 64;

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;
  uint32_t queued_frames_;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;

  rex::thread::global_critical_region global_critical_region_;
  static const size_t kMaximumClientCount = 8;
  struct {
    AudioDriver* driver;
    uint32_t callback;
    uint32_t callback_arg;
    uint32_t wrapped_callback_arg;
    bool in_use;
  } clients_[kMaximumClientCount];

  int FindFreeClient();

  std::unique_ptr<rex::thread::Semaphore> client_semaphores_[kMaximumClientCount];
  // Frames each client may have queued (its semaphore's permits in all), and
  // when its callback may run next (the worker paces them, WorkerThreadMain).
  // Both kept by the worker and the registration under the global lock.
  uint32_t permits_[kMaximumClientCount] = {};
  std::chrono::steady_clock::time_point next_callback_[kMaximumClientCount] = {};
  static constexpr std::chrono::nanoseconds kFramePeriod{5333333};  // 256 samples at 48 kHz
  // Event is always there in case we have no clients.
  std::unique_ptr<rex::thread::Event> shutdown_event_;
  rex::thread::WaitHandle* wait_handles_[kMaximumClientCount + 1];

  bool paused_ = false;
  rex::thread::Fence pause_fence_;
  std::unique_ptr<rex::thread::Event> resume_event_;
};

}  // namespace rex::audio
