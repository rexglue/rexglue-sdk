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

#pragma once

#include <memory>
#include <string>

#include <rex/ui/graphics_provider.h>

#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
#else
typedef void* id;
#endif

namespace rex {
namespace ui {
namespace metal {

// ============================================================================
// GPU Capabilities — queried once at init, used everywhere
// ============================================================================
struct MetalGpuCapabilities {
  // --- Device identity ---
  bool is_apple_gpu = false;       // Apple Silicon (M1/M2/M3/...)
  bool has_unified_memory = false;  // Shared CPU/GPU address space
  uint32_t gpu_family = 0;         // Apple GPU family (1-9) or Mac family
  GraphicsProvider::GpuVendorID vendor_id = GraphicsProvider::GpuVendorID::kApple;
  std::string device_name;

  // --- Memory ---
  size_t max_buffer_length = 0;          // Max single MTLBuffer size
  size_t recommended_working_set = 0;    // Recommended GPU memory usage

  // --- Feature support ---
  bool supports_raster_order_groups = false;  // ROGs for ROV emulation
  bool supports_tile_shading = false;         // Tile shading (imageblocks)
  bool supports_simd_scoped_operations = false;
  bool supports_barycentric_coordinates = false;
  bool supports_32bit_msaa = false;
  bool supports_32bit_float_filtering = false;
  bool supports_bc_texture_compression = false;
  bool supports_pull_model_interpolation = false;

  // --- Limits ---
  uint32_t max_texture_width_2d = 8192;
  uint32_t max_texture_height_2d = 8192;
  uint32_t max_texture_width_cube = 8192;
  uint32_t max_threads_per_threadgroup_dimension = 512;
  uint32_t max_threadgroup_memory_length = 16384;  // bytes
  uint32_t max_vertex_attributes = 31;
  uint32_t max_color_render_targets = 4;
  uint32_t max_total_color_render_target_size = 32;  // bytes

  // --- MSAA ---
  bool supports_msaa_2x = false;
  bool supports_msaa_4x = false;
  bool supports_msaa_8x = false;

  // --- Derived helpers ---
  bool CanUseTileShadingForEdram() const {
    return is_apple_gpu && supports_tile_shading;
  }
  bool CanUseRasterOrderGroups() const {
    return supports_raster_order_groups;
  }
  bool PreferSharedStorage() const {
    return has_unified_memory;
  }
};

class MetalProvider : public GraphicsProvider {
 public:
  ~MetalProvider() override;

  static std::unique_ptr<MetalProvider> Create(bool with_gpu_emulation,
                                               bool with_presentation);

  static bool IsAvailable();

#ifdef __OBJC__
  id<MTLDevice> device() const { return device_; }
  id<MTLCommandQueue> command_queue() const { return command_queue_; }
#else
  void* device() const { return device_; }
  void* command_queue() const { return command_queue_; }
#endif

  // Full capabilities struct — preferred over individual accessors.
  const MetalGpuCapabilities& caps() const { return caps_; }

  // Legacy accessors (delegate to caps_).
  bool is_apple_gpu() const { return caps_.is_apple_gpu; }
  bool has_unified_memory() const { return caps_.has_unified_memory; }
  size_t max_buffer_length() const { return caps_.max_buffer_length; }
  uint32_t gpu_family() const { return caps_.gpu_family; }
  GraphicsProvider::GpuVendorID GetAdapterVendorID() const { return caps_.vendor_id; }

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  // --- Debug / Profiling ---

  // Begin a Metal GPU capture (for Xcode GPU debugger).
  // Returns true if capture was started successfully.
  bool BeginGpuCapture();
  void EndGpuCapture();
  bool IsCapturing() const { return is_capturing_; }

  // Log a summary of GPU capabilities to the log output.
  void LogCapabilities() const;

 private:
  MetalProvider() = default;
  bool Initialize(bool with_gpu_emulation, bool with_presentation);
  void DetectCapabilities();

#ifdef __OBJC__
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> command_queue_ = nil;
#else
  void* device_ = nullptr;
  void* command_queue_ = nullptr;
#endif

  MetalGpuCapabilities caps_;
  bool is_capturing_ = false;
};

}  // namespace metal
}  // namespace ui
}  // namespace rex
