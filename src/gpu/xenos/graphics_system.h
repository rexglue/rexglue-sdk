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

#include <rex/runtime/function_dispatcher.h>

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

class GraphicsSystem : public gpu::IGPUBackend {
 public:
  virtual ~GraphicsSystem();

  virtual std::string GetName() const = 0;

  runtime::Memory* GetMemory() const { return memory_; }
  runtime::FunctionDispatcher* GetFunctionDispatcher() const { return function_dispatcher_; }
  /**
   * Returns the runtime the GPU workers belong to.
   *
   * @return The runtime, null before SetupGuestGPU.
   */
  Runtime* GetRuntime() const { return runtime_; }
  gpu::IPresenter* GetPresenter() const override { return presenter_.get(); }
  ::rex::gpu::Presenter* GetHostPresenter() const { return presenter_.get(); }
  ::rex::gpu::GraphicsProvider* GetProvider() const { return provider_.get(); }

  [[nodiscard]] Result<void> SetupPresentation(gpu::IUIThread& ui_thread) override;
  [[nodiscard]] Result<void> SetupGuestGPU(runtime::FunctionDispatcher& dispatcher,
                                           Runtime& runtime) override;
  bool HasPresentation() const override { return presenter_ != nullptr; }
  [[nodiscard]] Result<std::unique_ptr<gpu::IPresenter>> CreatePresenter() override;
  [[nodiscard]] Result<std::unique_ptr<gpu::IOverlayDrawer>> CreateOverlayDrawer(
      gpu::IPresenter& presenter) override;
  /**
   * Sets the backend config the module received at Connect. Null until set.
   *
   * @param config The backend config.
   */
  void SetConfig(const gpu::GPUConfig* config) { config_ = config; }
  const gpu::GPUConfig* GetConfig() const { return config_; }
  void Shutdown() override;

  /**
   * Reports a host GPU loss. May be called from any thread any number of times, even during
   * recovery from a device loss.
   *
   * @param is_responsible True when the caller is responsible for the recovery.
   */
  void OnHostGPULossFromAnyThread(bool is_responsible);

  xenos::RegisterFile* GetRegisterFile() { return &register_file_; }
  CommandProcessor* GetCommandProcessor() const { return command_processor_.get(); }

  void InitializeRingBuffer(u32 ptr, u32 size_log2) override;
  void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) override;

  void SetInterruptCallback(u32 callback, u32 user_data) override;
  void DispatchInterruptCallback(u32 source, u32 cpu);

  virtual void ClearCaches();
  virtual void InvalidateGPUMemory();

  [[nodiscard]] Result<void> InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking) override;

 protected:
  GraphicsSystem();

  /**
   * Builds the backend provider. Called lazily from either setup entry point.
   *
   * @param with_presentation False only on headless guest GPU paths.
   */
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static u32 ReadRegisterThunk(void* context, u32 addr);
  static void WriteRegisterThunk(void* context, u32 addr, u32 value);

  u32 ReadRegister(u32 addr);
  void WriteRegister(u32 addr, u32 value);

  void MarkVblank();

  runtime::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  Runtime* runtime_ = nullptr;
  gpu::IUIThread* ui_thread_ = nullptr;
  std::unique_ptr<::rex::gpu::GraphicsProvider> provider_;
  const gpu::GPUConfig* config_ = nullptr;
  bool provider_supports_presentation_ = false;

  std::atomic<u64> interrupt_callback_{0};

  std::atomic<bool> vsync_worker_running_;
  X_HANDLE vsync_worker_thread_ = 0;

  xenos::RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

 private:
  std::unique_ptr<::rex::gpu::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::gpu
