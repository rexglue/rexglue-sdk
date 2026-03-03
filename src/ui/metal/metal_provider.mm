/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 *              Phase 4: GPU capability detection, profiling support
 */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <rex/ui/metal/metal_provider.h>
#include <rex/ui/metal/metal_presenter.h>
#include <rex/ui/metal/metal_immediate_drawer.h>
#include <rex/logging.h>

namespace rex {
namespace ui {
namespace metal {

MetalProvider::~MetalProvider() {
  if (is_capturing_) {
    EndGpuCapture();
  }
  command_queue_ = nil;
  device_ = nil;
}

bool MetalProvider::IsAvailable() {
  NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
  bool available = devices != nil && devices.count > 0;
  return available;
}

std::unique_ptr<MetalProvider> MetalProvider::Create(bool with_gpu_emulation,
                                                     bool with_presentation) {
  auto provider = std::unique_ptr<MetalProvider>(new MetalProvider());
  if (!provider->Initialize(with_gpu_emulation, with_presentation)) {
    return nullptr;
  }
  return provider;
}

bool MetalProvider::Initialize(bool with_gpu_emulation,
                               bool with_presentation) {
  device_ = MTLCreateSystemDefaultDevice();
  if (!device_) {
    REXLOG_ERROR("MetalProvider: Failed to create Metal device");
    return false;
  }

  // Detect all capabilities first.
  DetectCapabilities();

  // Create the command queue.
  // Use a higher max command buffer count for emulation workloads.
  command_queue_ = [device_ newCommandQueueWithMaxCommandBufferCount:64];
  if (!command_queue_) {
    // Fallback to default.
    command_queue_ = [device_ newCommandQueue];
  }
  if (!command_queue_) {
    REXLOG_ERROR("MetalProvider: Failed to create command queue");
    return false;
  }

  LogCapabilities();
  return true;
}

void MetalProvider::DetectCapabilities() {
  auto& c = caps_;

  // --- Device identity ---
  c.device_name = [[device_ name] UTF8String];

  if (@available(macOS 10.15, *)) {
    c.is_apple_gpu = [device_ supportsFamily:MTLGPUFamilyApple1];
    c.has_unified_memory = [device_ hasUnifiedMemory];
  } else {
    c.is_apple_gpu = false;
    c.has_unified_memory = false;
  }

  c.max_buffer_length = [device_ maxBufferLength];

  // Recommended working set (total GPU-addressable memory budget).
  if (@available(macOS 10.13, *)) {
    c.recommended_working_set = [device_ recommendedMaxWorkingSetSize];
  } else {
    c.recommended_working_set = 0;
  }

  // --- GPU Family ---
  if (@available(macOS 10.15, *)) {
    if ([device_ supportsFamily:MTLGPUFamilyApple9]) {
      c.gpu_family = 9;  // M3
    } else if ([device_ supportsFamily:MTLGPUFamilyApple8]) {
      c.gpu_family = 8;  // M2
    } else if ([device_ supportsFamily:MTLGPUFamilyApple7]) {
      c.gpu_family = 7;  // M1
    } else if ([device_ supportsFamily:MTLGPUFamilyApple6]) {
      c.gpu_family = 6;
    } else if ([device_ supportsFamily:MTLGPUFamilyApple5]) {
      c.gpu_family = 5;
    } else if ([device_ supportsFamily:MTLGPUFamilyApple4]) {
      c.gpu_family = 4;
    } else if ([device_ supportsFamily:MTLGPUFamilyApple3]) {
      c.gpu_family = 3;
    } else if ([device_ supportsFamily:MTLGPUFamilyMac2]) {
      c.gpu_family = 2;
    } else {
      c.gpu_family = 1;
    }
  } else {
    c.gpu_family = 1;
  }

  // --- Vendor ID ---
  if (c.is_apple_gpu) {
    c.vendor_id = GraphicsProvider::GpuVendorID::kApple;
  } else {
    NSString* name = [device_ name];
    if ([name containsString:@"AMD"] || [name containsString:@"Radeon"]) {
      c.vendor_id = GraphicsProvider::GpuVendorID::kAMD;
    } else if ([name containsString:@"Intel"]) {
      c.vendor_id = GraphicsProvider::GpuVendorID::kIntel;
    } else if ([name containsString:@"NVIDIA"] ||
               [name containsString:@"GeForce"]) {
      c.vendor_id = GraphicsProvider::GpuVendorID::kNvidia;
    } else {
      c.vendor_id = GraphicsProvider::GpuVendorID::kApple;
    }
  }

  // --- Feature detection ---

  // Raster order groups: Apple GPU family 4+ (A11+, all Apple Silicon Macs).
  if (@available(macOS 10.15, *)) {
    c.supports_raster_order_groups =
        [device_ supportsFamily:MTLGPUFamilyApple4];
  }

  // Tile shading (imageblocks, tile memory): Apple GPU family 4+.
  // This is an Apple Silicon feature — Intel Macs do NOT support this.
  if (@available(macOS 10.15, *)) {
    c.supports_tile_shading = [device_ supportsFamily:MTLGPUFamilyApple4];
  }

  // SIMD-scoped operations: Apple GPU family 4+.
  if (@available(macOS 10.15, *)) {
    c.supports_simd_scoped_operations =
        [device_ supportsFamily:MTLGPUFamilyApple4];
  }

  // Barycentric coordinates: Apple GPU family 5+, or Mac family 2.
  if (@available(macOS 10.15, *)) {
    c.supports_barycentric_coordinates =
        (c.is_apple_gpu && [device_ supportsFamily:MTLGPUFamilyApple5]) ||
        [device_ supportsFamily:MTLGPUFamilyMac2];
  }

  // 32-bit MSAA.
  if (@available(macOS 10.15, *)) {
    c.supports_32bit_msaa = [device_ supportsFamily:MTLGPUFamilyApple7];
    if (!c.supports_32bit_msaa) {
      c.supports_32bit_msaa = [device_ supportsFamily:MTLGPUFamilyMac2];
    }
  }

  // 32-bit float filtering.
  c.supports_32bit_float_filtering = c.is_apple_gpu && c.gpu_family >= 7;

  // BC texture compression (DXT1/3/5): supported on Mac family 1+.
  if (@available(macOS 10.15, *)) {
    c.supports_bc_texture_compression =
        [device_ supportsFamily:MTLGPUFamilyMac1] ||
        [device_ supportsFamily:MTLGPUFamilyMac2];
    // Also Apple 7+.
    if (!c.supports_bc_texture_compression) {
      c.supports_bc_texture_compression =
          [device_ supportsFamily:MTLGPUFamilyApple7];
    }
  }

  // Pull-model interpolation.
  if (@available(macOS 10.15, *)) {
    c.supports_pull_model_interpolation =
        [device_ supportsFamily:MTLGPUFamilyMac2] ||
        (c.is_apple_gpu && c.gpu_family >= 5);
  }

  // --- Limits ---
  if (c.gpu_family >= 3 || !c.is_apple_gpu) {
    // Apple GPU family 3+ and all Mac/discrete GPUs support 16384.
    c.max_texture_width_2d = 16384;
    c.max_texture_height_2d = 16384;
    c.max_texture_width_cube = 16384;
  } else {
    c.max_texture_width_2d = 8192;
    c.max_texture_height_2d = 8192;
    c.max_texture_width_cube = 8192;
  }

  // Thread limits — query the device for the actual value.
  c.max_threads_per_threadgroup_dimension =
      (uint32_t)[device_ maxThreadsPerThreadgroup].width;
  c.max_threadgroup_memory_length =
      (uint32_t)[device_ maxThreadgroupMemoryLength];

  c.max_vertex_attributes = 31;
  c.max_color_render_targets = 8;  // Metal supports up to 8 MRT.
  c.max_total_color_render_target_size = 32;

  // --- MSAA ---
  c.supports_msaa_2x = [device_ supportsTextureSampleCount:2];
  c.supports_msaa_4x = [device_ supportsTextureSampleCount:4];
  c.supports_msaa_8x = [device_ supportsTextureSampleCount:8];
}

void MetalProvider::LogCapabilities() const {
  auto& c = caps_;
  REXLOG_INFO("MetalProvider: Device: {}", c.device_name);
  REXLOG_INFO("MetalProvider: Apple GPU: {}, Unified memory: {}, GPU Family: {}",
         c.is_apple_gpu ? "yes" : "no",
         c.has_unified_memory ? "yes" : "no",
         c.gpu_family);
  REXLOG_INFO("MetalProvider: Max buffer: {} MB, Working set: {} MB",
         c.max_buffer_length / (1024 * 1024),
         c.recommended_working_set / (1024 * 1024));
  REXLOG_INFO("MetalProvider: Features: ROG={}, TileShading={}, SIMD={}, "
         "BC={}, MSAA={}x",
         c.supports_raster_order_groups ? "yes" : "no",
         c.supports_tile_shading ? "yes" : "no",
         c.supports_simd_scoped_operations ? "yes" : "no",
         c.supports_bc_texture_compression ? "yes" : "no",
         c.supports_msaa_8x ? 8 : (c.supports_msaa_4x ? 4 :
                                    (c.supports_msaa_2x ? 2 : 1)));
  REXLOG_INFO("MetalProvider: Max texture: {}x{}, Max threads/group: {}",
         c.max_texture_width_2d, c.max_texture_height_2d,
         c.max_threads_per_threadgroup_dimension);

  if (c.CanUseTileShadingForEdram()) {
    REXLOG_INFO("MetalProvider: Will use tile shading for EDRAM emulation");
  } else {
    REXLOG_INFO("MetalProvider: Using standard buffer for EDRAM emulation");
  }
}

std::unique_ptr<Presenter> MetalProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return MetalPresenter::Create(*this, std::move(host_gpu_loss_callback));
}

std::unique_ptr<ImmediateDrawer> MetalProvider::CreateImmediateDrawer() {
  return MetalImmediateDrawer::Create(*this);
}

// ============================================================================
// Debug / Profiling
// ============================================================================

bool MetalProvider::BeginGpuCapture() {
  if (is_capturing_) return true;

  if (@available(macOS 10.15, *)) {
    MTLCaptureManager* capture_manager =
        [MTLCaptureManager sharedCaptureManager];
    if (![capture_manager supportsDestination:
            MTLCaptureDestinationGPUTraceDocument]) {
      REXLOG_WARN("MetalProvider: GPU capture to trace document not supported");
      // Try developer tools destination instead.
    }

    MTLCaptureDescriptor* capture_desc =
        [[MTLCaptureDescriptor alloc] init];
    capture_desc.captureObject = device_;
    capture_desc.destination = MTLCaptureDestinationDeveloperTools;

    NSError* error = nil;
    if ([capture_manager startCaptureWithDescriptor:capture_desc
                                              error:&error]) {
      is_capturing_ = true;
      REXLOG_INFO("MetalProvider: GPU capture started");
      return true;
    } else {
      REXLOG_WARN("MetalProvider: Failed to start GPU capture: {}",
             error ? [[error localizedDescription] UTF8String] : "unknown");
      return false;
    }
  }

  REXLOG_WARN("MetalProvider: GPU capture requires macOS 10.15+");
  return false;
}

void MetalProvider::EndGpuCapture() {
  if (!is_capturing_) return;

  if (@available(macOS 10.15, *)) {
    MTLCaptureManager* capture_manager =
        [MTLCaptureManager sharedCaptureManager];
    [capture_manager stopCapture];
    is_capturing_ = false;
    REXLOG_INFO("MetalProvider: GPU capture stopped");
  }
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
