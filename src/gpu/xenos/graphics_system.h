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

#pragma once

#include "rex/runtime/function_dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/gpu.h>
#include <rex/xenos/register_file.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/runtime/xthread.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/presenter.h"

namespace rex::gpu {

class CommandProcessor;

class GraphicsSystem : public gpu::IBackend {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  runtime::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  /**
   * Returns the runtime the GPU workers belong to.
   *
   * @return The runtime, null before SetupGuestGPU.
   */
  Runtime* runtime() const { return runtime_; }
  gpu::IPresenter* presenter() const override { return presenter_.get(); }
  ::rex::gpu::Presenter* host_presenter() const { return presenter_.get(); }
  ::rex::gpu::GraphicsProvider* provider() const { return provider_.get(); }

  [[nodiscard]] Result<void> SetupPresentation(gpu::IUIThread& ui_thread) override;
  [[nodiscard]] Result<void> SetupGuestGPU(runtime::FunctionDispatcher& dispatcher,
                                           Runtime& runtime) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  [[nodiscard]] Result<std::unique_ptr<gpu::IPresenter>> CreatePresenter() override;
  [[nodiscard]] Result<std::unique_ptr<gpu::IOverlayDrawer>> CreateOverlayDrawer(
      gpu::IPresenter& presenter) override;
  /** The backend slice the module received at Connect. Null until set. */
  void set_config(const gpu::GPUConfig* config) { config_ = config; }
  const gpu::GPUConfig* config() const { return config_; }
  void Shutdown() override;

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  xenos::RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }

  void InitializeRingBuffer(u32 ptr, u32 size_log2) override;
  void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) override;

  void SetInterruptCallback(u32 callback, u32 user_data) override;
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  [[nodiscard]] Result<void> InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking) override;

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  GraphicsSystem();

  // Backends build their provider here. Called lazily from either setup
  // entry point; with_presentation is false only on headless guest-GPU paths.
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static u32 ReadRegisterThunk(void* context, u32 addr);
  static void WriteRegisterThunk(void* context, u32 addr, u32 value);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  runtime::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  Runtime* runtime_ = nullptr;
  gpu::IUIThread* ui_thread_ = nullptr;
  std::unique_ptr<::rex::gpu::GraphicsProvider> provider_;
  const gpu::GPUConfig* config_ = nullptr;
  bool provider_supports_presentation_ = false;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> vsync_worker_running_;
  X_HANDLE vsync_worker_thread_ = 0;

  xenos::RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

 private:
  std::unique_ptr<::rex::gpu::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::gpu
