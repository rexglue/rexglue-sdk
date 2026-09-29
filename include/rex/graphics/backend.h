/**
 * @file        graphics/backend.h
 * @brief       Graphics backend interface served by graphics modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <rex/macros.h>
#include <rex/system/xtypes.h>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui
namespace rex::system {
class KernelState;
}

namespace rex::graphics {

/**
 * Structural configuration for a graphics backend module.
 */
struct BackendConfig {
  /**
   * Backend to construct. "d3d12", "vulkan" or "any". "any" picks D3D12 when available,
   * then Vulkan. Validated in the module's Init.
   */
  std::string backend = "any";
  /** D3D12 adapter index. -1 picks the first hardware adapter, -2 picks WARP. */
  int32_t d3d12_adapter = -1;
  /** Vulkan physical device index. -1 picks automatically. */
  int32_t vulkan_device = -1;
  /** Default of gpu.resolution_scale_x. 1 to 8. */
  int32_t resolution_scale_x = 1;
  /** Default of gpu.resolution_scale_y. 1 to 8. */
  int32_t resolution_scale_y = 1;
};

/**
 * A graphics backend. Served as "rex.graphics.backend" version 1 by graphics modules.
 */
class IGraphicsBackend {
 public:
  REX_INTERFACE(IGraphicsBackend, "rex.graphics.backend", 1);

  virtual ~IGraphicsBackend() = default;

  /**
   * Builds the provider and presenter. Safe to call without a Runtime to stand up a
   * window and ImGui. Idempotent. Must precede SetupGuestGPU when presentation is wanted,
   * because some backends bake swapchain support into the provider.
   *
   * @param app_context The windowed app context that owns the UI thread.
   * @return X_STATUS_SUCCESS or the backend's failure status.
   */
  virtual X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) = 0;

  /**
   * Wires the GPU into the guest address space, MMIO, command processor and vsync worker.
   * Builds a headless provider when SetupPresentation was not called.
   *
   * @param function_dispatcher The runtime's dispatcher.
   * @param kernel_state The runtime's kernel state.
   * @return X_STATUS_SUCCESS or the backend's failure status.
   */
  virtual X_STATUS SetupGuestGPU(runtime::FunctionDispatcher* function_dispatcher,
                                 system::KernelState* kernel_state) = 0;

  /**
   * @return True once SetupPresentation has built a presenter.
   */
  virtual bool has_presentation() const = 0;

  /**
   * @return The host graphics provider, or null for backends without one.
   */
  virtual ui::GraphicsProvider* provider() const { return nullptr; }

  /**
   * @return The host presenter, or null for backends without one.
   */
  virtual ui::Presenter* presenter() const { return nullptr; }

  /**
   * Registers the guest interrupt callback reached from VdSetGraphicsInterruptCallback.
   *
   * @param callback Guest function address.
   * @param user_data Guest pointer passed back to the callback.
   */
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    (void)callback;
    (void)user_data;
  }

  /**
   * Sets the ring buffer reached from VdInitializeRingBuffer.
   *
   * @param ptr Physical address of the ring buffer.
   * @param size_log2 Log2 of the ring buffer size.
   */
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    (void)ptr;
    (void)size_log2;
  }

  /**
   * Enables read pointer write back reached from VdEnableRingBufferRPtrWriteBack.
   *
   * @param ptr Physical address that receives the read pointer.
   * @param block_size_log2 Log2 of the write back block size.
   */
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    (void)ptr;
    (void)block_size_log2;
  }

  /**
   * Opens persistent shader and pipeline storage under the cache root.
   *
   * @param cache_root Directory that holds the storage.
   * @param title_id Title the storage belongs to.
   * @param blocking True to load the storage before returning.
   */
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking) {
    (void)cache_root;
    (void)title_id;
    (void)blocking;
  }

  /**
   * Runs SetupPresentation when wanted and not done, then SetupGuestGPU.
   *
   * @param function_dispatcher The runtime's dispatcher.
   * @param kernel_state The runtime's kernel state.
   * @param app_context The windowed app context, used only when with_presentation is true.
   * @param with_presentation True to build presentation first.
   * @return X_STATUS_SUCCESS or the first failure status.
   */
  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher,
                 system::KernelState* kernel_state, ui::WindowedAppContext* app_context,
                 bool with_presentation) {
    if (with_presentation && !has_presentation()) {
      X_STATUS status = SetupPresentation(app_context);
      if (XFAILED(status)) {
        return status;
      }
    }
    return SetupGuestGPU(function_dispatcher, kernel_state);
  }

  /**
   * Tears down the guest GPU state and presentation. Does not destroy the object.
   */
  virtual void Shutdown() = 0;
};

}  // namespace rex::graphics
