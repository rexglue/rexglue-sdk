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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/logging/log_category.h>
#include <rex/memory.h>
#include <rex/guest.h>
#include <rex/title.h>
#include <rex/utility.h>
#include <rex/types.h>
#include <rex/xenos/register.h>
#include <rex/settings_registry.h>
#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/host/xenos_presentation.h"
#include "gpu/xenos/xenos_settings.h"
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

class CommandProcessor;
class GraphicsSettings;

/**
 * Shared base of the Xenos drivers. A host API derives from it and supplies the provider, the
 * command processor and the presentation.
 *
 * AttachTitle starts the command processor and the vblank thread. DetachTitle stops them.
 */
class REX_GPU_XENOS_API XenosGraphicsDriverBase : public IXenosGraphicsDriver,
                                                  public XenosSettings {
 public:
  ~XenosGraphicsDriverBase() override;

  /**
   * Hands the graphics settings to the drivers this library creates. A provider module calls it
   * at Connect.
   *
   * @param settings The settings, or null to release them at Disconnect.
   */
  static void SetGraphicsSettings(GraphicsSettings* settings);

  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;
  /**
   * Builds the provider, the presenter and the drawer for the window. A driver without a window
   * creates its provider at AttachTitle.
   *
   * @return Ok, or the failure of the provider or presenter.
   */
  [[nodiscard]] Result<void> SetSurface(const NativeWindow& window) override;
  [[nodiscard]] Result<void> AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                         IPhysicalWriteWatch& watch,
                                         IGuestInterrupts& interrupts) override;
  void DetachTitle() override;

  FrameStatus WaitForFrame(u32 timeout_ms) override;
  void ResizeSurface(u32 width, u32 height) override;
  [[nodiscard]] bool BeginPresent() override;
  void EndPresent() override;
  void RenderOverlays(ImDrawData& draw_data) override;
  void InitializeRingBuffer(u32 address, u32 size_log2) override;
  void EnableReadPointerWriteBack(u32 address, u32 block_size_log2) override;
  u32 ReadRegister(u32 index) override;
  void WriteRegister(u32 index, u32 value) override;

  ::rex::gpu::Presenter* GetHostPresenter() const {
    return presentation_ ? presentation_->GetPresenter() : nullptr;
  }
  ::rex::gpu::GraphicsProvider* GetProvider() const { return provider_.get(); }

  /**
   * Reports a host GPU loss. Callable from any thread, repeatedly.
   *
   * @param is_responsible True when the caller is responsible for the recovery.
   */
  void OnHostGPULossFromAnyThread(bool is_responsible);

  /** Reports a finished swap to WaitForFrame. Command processor thread only. */
  void NotifySwap();

  xenos::RegisterFile* GetRegisterFile() { return &register_file_; }
  CommandProcessor* GetCommandProcessor() const { return command_processor_.get(); }

  void DispatchInterruptCallback(u32 source, u32 cpu);

  /**
   * Reads gpu.vsync.
   *
   * @return True when presents and the vblank wait for the display refresh.
   */
  bool IsVSyncEnabled() const;

  /**
   * Reads gpu.resolution_scale_x.
   *
   * @return The draw resolution scale on X, at least one.
   */
  u32 GetResolutionScaleX() const;

  /**
   * Reads gpu.resolution_scale_y.
   *
   * @return The draw resolution scale on Y, at least one.
   */
  u32 GetResolutionScaleY() const;

  /**
   * Reads gpu.anisotropic_override.
   *
   * @return The anisotropic filtering level, or a negative value to follow the title.
   */
  i32 GetAnisotropicOverride() const;

  virtual void ClearCaches();
  virtual void InvalidateGPUMemory();

  /**
   * Opens persistent shader and pipeline storage unless it is open.
   *
   * @param root Directory that holds the storage.
   * @param blocking True to load the storage before returning.
   * @return Ok, or the failure to open the storage.
   */
  [[nodiscard]] Result<void> InitializeShaderStorage(const std::filesystem::path& root,
                                                     u32 title_id, bool blocking);

 protected:
  XenosGraphicsDriverBase();

  /**
   * Builds the driver provider.
   *
   * @param with_presentation False only on headless guest GPU paths.
   */
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  void MarkVblank();

  TitleInfo title_info_;
  IPhysicalMemory* physical_memory_ = nullptr;
  IPhysicalWriteWatch* write_watch_ = nullptr;
  IGuestInterrupts* guest_interrupts_ = nullptr;
  std::unique_ptr<::rex::gpu::GraphicsProvider> provider_;

  ISettingsRegistry* settings_ = nullptr;
  GraphicsSettings* graphics_settings_ = nullptr;
  static inline GraphicsSettings* provided_settings_ = nullptr;
  std::atomic<bool> vsync_worker_running_;
  ScopeGuard vsync_worker_thread_;

  xenos::RegisterFile register_file_;
  rex::log::OncePerKey<xenos::RegisterFile::kRegisterCount> unknown_read_once_;
  rex::log::OncePerKey<xenos::RegisterFile::kRegisterCount> unknown_write_once_;
  std::unique_ptr<CommandProcessor> command_processor_;

 private:
  std::unique_ptr<XenosPresentation> presentation_;

  std::atomic_flag host_gpu_loss_reported_;
  bool shader_storage_opened_ = false;
  std::mutex frame_mutex_;
  std::condition_variable frame_signal_;
  u64 swap_count_ = 0;
  u64 swap_seen_ = 0;
};

}  // namespace rex::gpu
