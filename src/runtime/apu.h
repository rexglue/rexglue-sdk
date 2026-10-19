/**
 * @file        runtime/apu.h
 * @brief       The APU, its render clients, its worker and the XMA decoder
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <rex/platform.h>
#include <rex/apu.h>
#include <rex/export.h>
#include <rex/platform/lock_order.h>
#include <rex/platform/thread.h>
#include <rex/result.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

class XMADecoder;

/** The APU. Owns 8 render client slots, the "Audio Worker" runtime thread and the XMA decoder. */
class REX_RUNTIME_API APU : public NonCopyable {
 public:
  /** Render client slots. */
  static constexpr u32 kMaxClients = 8;

  /**
   * Creates the APU.
   *
   * @param runtime The runtime the worker belongs to.
   * @param backend The audio output the clients play to.
   * @param max_queued_frames Credits each client starts with, 4 to 64.
   */
  APU(Runtime& runtime, apu::IAPUBackend& backend, u32 max_queued_frames);

  /** Shuts down. */
  ~APU();

  /**
   * Starts the XMA decoder and the audio worker.
   *
   * @return Ok, or a Runtime error with the decoder status as its code.
   */
  [[nodiscard]] Result<void> Setup();

  /** Stops the worker and the decoder and waits for the worker to end. */
  void StopWorker();

  /** Stops the worker and the decoder and closes every client stream. */
  void Shutdown();

  /**
   * Registers a render client and opens its stream.
   *
   * @param callback Guest function the worker calls once per credit.
   * @param callback_arg Title argument passed to the callback.
   * @return The slot index, or a Runtime error with code X_STATUS_NO_MEMORY when 8 clients are
   *         registered or X_STATUS_UNSUCCESSFUL when the backend refuses the stream.
   */
  [[nodiscard]] Result<u32> RegisterClient(u32 callback, u32 callback_arg);

  /**
   * Closes a client stream and drains its credits.
   *
   * @param index A slot index from RegisterClient.
   */
  void UnregisterClient(u32 index);

  /**
   * Submits a title frame of a client.
   *
   * @param index A slot index from RegisterClient.
   * @param samples_ptr Guest address of the frame.
   */
  void SubmitFrame(u32 index, u32 samples_ptr);

  /**
   * Parks or resumes the worker and the XMA decoder. A repeated value returns at once.
   *
   * @param paused True to park.
   */
  void SetPaused(bool paused);

  /**
   * Reports whether the APU is paused.
   *
   * @return True while paused.
   */
  bool IsPaused() const { return paused_.load(); }

  /**
   * Returns the XMA decoder.
   *
   * @return The decoder.
   */
  XMADecoder& GetXMADecoder() { return *xma_decoder_; }

  /**
   * Returns the audio output the clients play to.
   *
   * @return The backend.
   */
  apu::IAPUBackend& GetBackend() { return backend_; }

  /**
   * Returns the credits a slot holds. Test seam.
   *
   * @param index A slot index.
   * @return The semaphore count.
   */
  u32 CountCredits(u32 index);

  /**
   * Takes a credit without waiting. Test seam.
   *
   * @param index A slot index.
   * @return True when a credit was taken.
   */
  bool TakeCredit(u32 index);

 private:
  struct Client {
    u32 stream = 0;
    u32 callback = 0;
    u32 callback_arg = 0;
    u32 wrapped_callback_arg = 0;
    bool in_use = false;
  };

  void WorkerMain();

  Runtime& runtime_;
  apu::IAPUBackend& backend_;
  u32 queued_frames_;
  std::unique_ptr<XMADecoder> xma_decoder_;
  std::atomic<bool> worker_running_{false};
  X_HANDLE worker_thread_ = 0;
  platform::RankedMutex mutex_{platform::LockRank::kObject};
  std::array<Client, kMaxClients> clients_{};
  std::array<std::unique_ptr<platform::Semaphore>, kMaxClients> semaphores_;
  std::unique_ptr<platform::Event> shutdown_event_;
  std::array<platform::WaitHandle*, kMaxClients + 1> wait_handles_{};
  std::mutex pause_mutex_;
  std::atomic<bool> paused_{false};
  bool parked_ = false;
  platform::Fence pause_fence_;
  std::unique_ptr<platform::Event> resume_event_;
};

}  // namespace rex::runtime
