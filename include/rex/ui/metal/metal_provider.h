/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_PROVIDER_H_
#define XENIA_UI_METAL_METAL_PROVIDER_H_

#include <memory>
#include <vector>

#include "rex/ui/graphics_provider.h"

namespace rex {
namespace ui {
namespace metal {

// Phase 2b.3 Metal graphics provider. Owns the MTLDevice and a command queue and
// vends the Metal presenter / immediate drawer. Metal objects are held as opaque
// pointers (retained via __bridge_retained) so this header stays plain C++.
class MetalProvider : public ::rex::gpu::GraphicsProvider {
 public:
  ~MetalProvider() override;

  // Creates the provider on SelectedDevice(). Returns null if no Metal device
  // is available.
  static std::unique_ptr<MetalProvider> Create();

  // The Metal device the emulator renders with: the system default, or the
  // device named by metal_device (Macs with more than one GPU, such as a Mac
  // Pro with two). An unretained id<MTLDevice>, valid for the process
  // lifetime; null if there is no Metal device.
  static void* SelectedDeviceForQueue();

  // id<MTLDevice> / id<MTLCommandQueue>.
  void* device() const { return device_; }
  void* command_queue() const { return command_queue_; }

  // GPU-fault recovery: after the driver revokes the queue (every submission
  // returns kIOGPUCommandBufferCallbackErrorSubmissionsIgnored, latched — the
  // display freezes while the emulator runs on), only a FRESH queue restores
  // submission. The old queue object is intentionally kept retained until
  // shutdown so a thread mid-use (the paint thread) dereferences a
  // dead-but-valid object instead of freed memory. Returns false on failure.
  bool RecreateQueue() override;

  // The provider base's host-device hook; the static picker above is for callers without an instance.
  void* SelectedDevice() override { return SelectedDeviceForQueue(); }
  void* command_queue() override;

  std::unique_ptr<::rex::gpu::Presenter> CreatePresenter(
      const ::rex::gpu::XenosGraphicsDriverBase& driver,
      ::rex::gpu::Presenter::HostGPULossCallback host_gpu_loss_callback =
          ::rex::gpu::Presenter::FatalErrorHostGPULossCallback) override;

  std::unique_ptr<::rex::gpu::OverlayDrawer> CreateImmediateDrawer() override;

 private:
  MetalProvider() = default;

  bool Initialize();

  void* device_ = nullptr;         // id<MTLDevice> (retained)
  void* command_queue_ = nullptr;  // id<MTLCommandQueue> (retained)
  // Queues revoked by the driver, kept alive until shutdown (see
  // RecreateQueue).
  std::vector<void*> retired_command_queues_;
};

}  // namespace metal
}  // namespace ui
}  // namespace rex

#endif  // XENIA_UI_METAL_METAL_PROVIDER_H_
