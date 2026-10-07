/**
 * @file        runtime/null_gpu.h
 * @brief       The GPU the runtime serves when no GPU module is loaded
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

#include <rex/gpu/backend.h>
#include <rex/macros.h>
#include <rex/mmio.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

/**
 * The null GPU. Raises the vblank interrupt at the refresh rate and writes the ring buffer write
 * pointer back as the read pointer. It executes no PM4, so a title that waits on a fence written
 * by a PM4 packet stops there. Headless title runs use rex-gpu-xenos with backend null instead.
 */
class REX_RUNTIME_API NullGPU final : public gpu::IBackend, public NonCopyable {
 public:
  ~NullGPU() override;

  [[nodiscard]] Result<void> SetupPresentation(gpu::IUIThread& ui_thread) override;
  [[nodiscard]] Result<void> SetupGuestGPU(FunctionDispatcher& dispatcher,
                                           Runtime& runtime) override;
  bool has_presentation() const override { return false; }
  gpu::IPresenter* presenter() const override { return nullptr; }
  [[nodiscard]] Result<std::unique_ptr<gpu::IPresenter>> CreatePresenter() override;
  [[nodiscard]] Result<std::unique_ptr<gpu::IOverlayDrawer>> CreateOverlayDrawer(
      gpu::IPresenter& presenter) override;
  void SetInterruptCallback(u32 callback, u32 user_data) override;
  void InitializeRingBuffer(u32 ptr, u32 size_log2) override;
  void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) override;
  [[nodiscard]] Result<void> InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking) override;
  void Shutdown() override;

 private:
  static u32 ReadRegisterThunk(void* context, u32 address);
  static void WriteRegisterThunk(void* context, u32 address, u32 value);
  u32 ReadRegister(u32 address) const;
  void WriteRegister(u32 address, u32 value);
  void RaiseVblank();

  FunctionDispatcher* dispatcher_ = nullptr;
  Runtime* runtime_ = nullptr;
  std::atomic<u32> callback_{0};
  std::atomic<u32> callback_data_{0};
  std::atomic<u32> read_pointer_writeback_{0};
  std::atomic<bool> vsync_running_{false};
  X_HANDLE vsync_thread_ = 0;
  bool mmio_attached_ = false;
  std::array<std::atomic<u32>, kMMIOBlockSize / 4> registers_{};
};

}  // namespace rex::runtime
