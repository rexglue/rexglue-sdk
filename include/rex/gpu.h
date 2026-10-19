/**
 * @file        gpu.h
 * @brief       GPU backend interface, presenter and surface types, and the GPU config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rex/platform.h>
#include <rex/gpu/overlay.h>
#include <rex/gpu/presenter.h>
#include <rex/gpu/surface.h>
#include <rex/gpu/ui_thread.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex {
class Runtime;
}  // namespace rex
namespace rex::runtime {
class FunctionDispatcher;
}  // namespace rex::runtime

namespace rex::gpu {

/** A GPU backend, served by a backend module. */
REX_INTERFACE(IGPUBackend, "rex.gpu.backend", 1)
 public:
  /**
   * Builds the device and the guest output presenter.
   *
   * @param ui_thread The thread that creates and destroys the presenter.
   * @return Ok, or the backend failure.
   */
  [[nodiscard]] virtual Result<void> SetupPresentation(IUIThread& ui_thread) = 0;

  /**
   * Connects the GPU to the guest address space, MMIO, the command processor and the vsync
   * worker. Builds a device without presentation when SetupPresentation did not run.
   *
   * @param dispatcher The runtime's dispatcher.
   * @param runtime The runtime the GPU workers belong to.
   * @return Ok, or the backend failure.
   */
  [[nodiscard]] virtual Result<void> SetupGPU(runtime::FunctionDispatcher& dispatcher,
                                              Runtime& runtime) = 0;

  /**
   * Reports whether SetupPresentation built the guest output presenter.
   *
   * @return True when the presenter exists.
   */
  virtual bool HasPresentation() const = 0;

  /**
   * Returns the guest output presenter, owned by the backend.
   *
   * @return The presenter, or null before SetupPresentation.
   */
  virtual IPresenter* GetPresenter() const = 0;

  /**
   * Creates a presenter for another window on the backend's device.
   *
   * @return The presenter, or the backend failure.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IPresenter>> CreatePresenter() = 0;

  /**
   * Creates an overlay drawer that submits to a presenter of this backend.
   *
   * @param presenter The presenter the drawer submits to. It outlives the drawer.
   * @return The drawer, or the backend failure.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IOverlayDrawer>> CreateOverlayDrawer(
      IPresenter& presenter) = 0;

  /**
   * Registers the guest interrupt callback reached from VdSetGraphicsInterruptCallback.
   *
   * @param callback Guest function address.
   * @param user_data Title pointer passed back to the callback.
   */
  virtual void SetInterruptCallback(u32 callback, u32 user_data) = 0;

  /**
   * Sets the ring buffer reached from VdInitializeRingBuffer.
   *
   * @param ptr Physical address of the ring buffer.
   * @param size_log2 Log2 of the ring buffer size.
   */
  virtual void InitializeRingBuffer(u32 ptr, u32 size_log2) = 0;

  /**
   * Enables read pointer write back reached from VdEnableRingBufferRPtrWriteBack.
   *
   * @param ptr Physical address that receives the read pointer.
   * @param block_size_log2 Log2 of the write back block size.
   */
  virtual void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2) = 0;

  /**
   * Opens persistent shader and pipeline storage under the cache root.
   *
   * @param root Directory that holds the storage.
   * @param title_id Title the storage belongs to.
   * @param blocking True to load the storage before returning.
   * @return Ok, or the failure to open the storage.
   */
  [[nodiscard]] virtual Result<void> InitializeShaderStorage(const std::filesystem::path& root,
                                                             u32 title_id, bool blocking) = 0;

  /** Tears down the GPU state and presentation. The object stays. */
  virtual void Shutdown() = 0;
};

/** Structural configuration for the GPU backend module. */
struct GPUConfig {
  /** Backend host API, "any", "d3d12", "vulkan" or "null". */
  std::string backend = "any";
  /** D3D12 adapter index. -1 picks the first hardware adapter, -2 picks WARP. */
  i32 d3d12_adapter = -1;
  /** Vulkan physical device index. -1 picks automatically. */
  i32 vulkan_device = -1;
  /** Default of gpu.resolution_scale_x, 1 to 8. */
  i32 resolution_scale_x = 1;
  /** Default of gpu.resolution_scale_y, 1 to 8. */
  i32 resolution_scale_y = 1;
};

}  // namespace rex::gpu
