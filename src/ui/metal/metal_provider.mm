/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/ui/metal/metal_provider.h"

#import <Metal/Metal.h>

#include <utility>
#include <string>

#include "rex/graphics/metal/metal_settings.h"
#include <rex/logging/log_category.h>
#include "rex/ui/metal/metal_alloc_census.h"
#include "rex/ui/metal/metal_immediate_drawer.h"
#include "rex/ui/metal/metal_presenter.h"


namespace rex {
namespace ui {
namespace metal {

void* MetalProvider::SelectedDeviceForQueue() {
  static id<MTLDevice> s_device = [] {
    id<MTLDevice> chosen = MTLCreateSystemDefaultDevice();
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    const std::string& want = ::rex::graphics::metal::GetMetalSettings().metal_device.Get();
    id<MTLDevice> named = nil;
    for (id<MTLDevice> device in devices) {
      const char* name = [[device name] UTF8String];
      REXLOG_INFO("MetalProvider: device \"{}\": {} MiB working set, {}, {}",
             name ? name : "unknown",
             uint64_t(device.recommendedMaxWorkingSetSize) >> 20,
             device.hasUnifiedMemory ? "unified memory" : "dedicated VRAM",
#if TARGET_OS_OSX
             device.lowPower ? "low power" : "high power"
#else
             // MTLDevice.lowPower is a macOS property; on iOS the power class is not exposed.
             "power class not reported"
#endif
      );
      if (!named && !want.empty() && name &&
          [[device name] rangeOfString:@(want.c_str())
                               options:NSCaseInsensitiveSearch]
                  .location != NSNotFound) {
        named = device;
      }
    }
    if (named) {
      chosen = named;
    } else if (!want.empty()) {
      REXLOG_WARN("MetalProvider: no device matches metal_device \"{}\"; using "
             "the system default",
             want);
    }
    MetalAllocCensusInstall((__bridge void*)chosen);
    return chosen;
  }();
  return (__bridge void*)s_device;
}

MetalProvider::~MetalProvider() {
  // Balance the __bridge_retained transfers from Initialize(): move ownership
  // back into ARC so the objects are released.
  if (command_queue_) {
    id<MTLCommandQueue> queue = (__bridge_transfer id<MTLCommandQueue>)command_queue_;
    (void)queue;
    command_queue_ = nullptr;
  }
  for (void* retired : retired_command_queues_) {
    id<MTLCommandQueue> queue = (__bridge_transfer id<MTLCommandQueue>)retired;
    (void)queue;
  }
  retired_command_queues_.clear();
  if (device_) {
    id<MTLDevice> device = (__bridge_transfer id<MTLDevice>)device_;
    (void)device;
    device_ = nullptr;
  }
}

std::unique_ptr<MetalProvider> MetalProvider::Create() {
  REXLOG_INFO("MetalProvider::Create: entering");
  std::unique_ptr<MetalProvider> provider(new MetalProvider());
  if (!provider->Initialize()) {
    return nullptr;
  }
  return provider;
}

bool MetalProvider::Initialize() {
  id<MTLDevice> device = (__bridge id<MTLDevice>)SelectedDevice();
  if (!device) {
    REXLOG_ERROR("MetalProvider: no Metal device");
    return false;
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (!queue) {
    REXLOG_ERROR("MetalProvider: failed to create command queue");
    return false;
  }
  // Transfer ownership out of ARC into the retained opaque pointers.
  device_ = (__bridge_retained void*)device;
  command_queue_ = (__bridge_retained void*)queue;
  REXLOG_INFO("MetalProvider: initialized on {}",
         [[device name] UTF8String] ? [[device name] UTF8String] : "unknown");
  return true;
}

bool MetalProvider::RecreateQueue() {
  if (!device_) {
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (!queue) {
    REXLOG_ERROR("MetalProvider: RecreateQueue failed to create a new queue");
    return false;
  }
  // Keep the revoked queue alive until shutdown (a thread mid-use must see a
  // dead-but-valid object, not freed memory).
  if (command_queue_) {
    retired_command_queues_.push_back(command_queue_);
  }
  command_queue_ = (__bridge_retained void*)queue;
  REXLOG_WARN("MetalProvider: command queue RECREATED after driver revocation ({} retired)",
         retired_command_queues_.size());
  return true;
}

std::unique_ptr<::rex::gpu::Presenter> MetalProvider::CreatePresenter(
    const ::rex::gpu::XenosGraphicsDriverBase& driver,
    ::rex::gpu::Presenter::HostGPULossCallback host_gpu_loss_callback) {
  return MetalPresenter::Create(std::move(host_gpu_loss_callback), driver, device_,
                                command_queue_);
}

std::unique_ptr<::rex::gpu::OverlayDrawer> MetalProvider::CreateImmediateDrawer() {
  auto drawer = std::make_unique<MetalImmediateDrawer>();
  if (!drawer->Initialize(device_)) {
    // Returning null leaves the UI without a drawer, which is the same
    // no-overlay state as before this drawer existed -- but it also means guest
    // system dialogs stay invisible and block their guest thread, so this is
    // loud rather than silent.
    REXLOG_ERROR(
        "MetalProvider: immediate drawer initialization failed; UI overlays "
        "and guest system dialogs will not render");
    return nullptr;
  }
  return drawer;
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
