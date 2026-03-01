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

#include "rex/system/processor.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/graphics/register_file.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

// Forward declarations
namespace rex {
class Memory;
namespace stream {
class ByteStream;
}  // namespace stream
}  // namespace rex

namespace rex::ui {
class WindowedAppContext;
}  // namespace rex::ui

namespace rex::graphics {

class CommandProcessor;

class GraphicsSystem : public system::IGraphicsSystem {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  memory::Memory* memory() const { return memory_; }
  runtime::Processor* processor() const { return processor_; }
  system::KernelState* kernel_state() const { return kernel_state_; }
  ::rex::ui::GraphicsProvider* provider() const { return provider_.get(); }
  ::rex::ui::Presenter* presenter() const { return presenter_.get(); }

  virtual X_STATUS Setup(runtime::Processor* processor, system::KernelState* kernel_state,
                         ::rex::ui::WindowedAppContext* app_context, bool with_presentation);
  virtual void Shutdown();

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }

  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data);
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);

  void RequestFrameTrace();
  void BeginTracing();
  void EndTracing();

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  GraphicsSystem();

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                 uint32_t value);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  memory::Memory* memory_ = nullptr;
  runtime::Processor* processor_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  ::rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<::rex::ui::GraphicsProvider> provider_;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> vsync_worker_running_;
  system::object_ref<system::XHostThread> vsync_worker_thread_;

  RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

 private:
  std::unique_ptr<::rex::ui::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::graphics
