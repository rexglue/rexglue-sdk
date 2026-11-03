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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/types.h>
#include "gpu/xenos/host/overlay.h"
#include "gpu/xenos/host/presentation.h"
#include "gpu/xenos/host/surface.h"
#include <rex/utility.h>

#include "gpu/xenos/host/present_policy.h"

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <windows.h>

#include <wrl/client.h>
#endif  // REX_PLATFORM_WIN32

#include "gpu/xenos/xenos_export.h"

namespace rex {
namespace gpu {

class Presenter;
class XenosGraphicsDriverBase;

/**
 * Displays the guest output image and the UI overlays on a host surface.
 *
 * The guest output image may be refreshed from any one thread at a time, usually the GPU
 * emulation thread. The UI overlays are managed by the UI thread.
 *
 * Only the UI thread manages the painting connection to the Surface. The refreshing thread may
 * mark the connection outdated and request painting, but must never reconnect itself.
 */
class REX_GPU_XENOS_API Presenter : public gpu::IPresenter {
 public:
  /**
   * Called on host GPU loss.
   *
   * May run on the UI thread even when statically_from_ui_thread is false, such as when the UI
   * thread refreshes the guest output.
   */
  using HostGPULossCallback =
      rex::MoveOnlyFunction<void(bool is_responsible, bool statically_from_ui_thread)>;
  static void FatalErrorHostGPULossCallback(bool is_responsible, bool statically_from_ui_thread);

  class GuestOutputRefreshContext : public rex::NonCopyable {
   public:
    virtual ~GuestOutputRefreshContext() = default;

    /**
     * Sets whether the source has at most 8 bits of precision per channel. Defaults to false.
     *
     * @param is_8bpc True when the source has at most 8 bits of precision.
     */
    void SetIs8bpc(bool is_8bpc) { is_8bpc_out_ref_ = is_8bpc; }

   protected:
    GuestOutputRefreshContext(bool& is_8bpc_out_ref) : is_8bpc_out_ref_(is_8bpc_out_ref) {
      is_8bpc_out_ref = false;
    }

   private:
    bool& is_8bpc_out_ref_;
  };

  class GuestOutputPaintConfig {
   public:
    bool GetAllowOverscanCutoff() const { return allow_overscan_cutoff_; }
    void SetAllowOverscanCutoff(bool new_allow_overscan_cutoff) {
      allow_overscan_cutoff_ = new_allow_overscan_cutoff;
    }

    /** Dithering removes banding in smooth gradients such as skies at 8bpc host output. */
    bool GetDither() const { return dither_; }
    void SetDither(bool new_dither) { dither_ = new_dither; }

   private:
    /** Tools must not allow overscan cutoff and must use the bilinear effect to match source. */
    bool allow_overscan_cutoff_ = false;
    bool dither_ = false;
  };

  virtual ~Presenter();

  bool Attach(gpu::IPresentationHost* host, gpu::Surface* surface) override {
    SetWindowSurfaceFromUIThread(host, surface);
    return surface_ != nullptr;
  }
  void OnSurfaceMonitorUpdate(bool old_monitor_potentially_disconnected) override {
    OnSurfaceMonitorUpdateFromUIThread(old_monitor_potentially_disconnected);
  }
  void OnSurfaceResize() override { OnSurfaceResizeFromUIThread(); }
  void Paint(bool force) override { PaintFromUIThread(force); }

  /**
   * Refreshes the guest output image through the refresher callback.
   *
   * A width or height of 0 disables guest output until the next refresh with a nonzero size. The
   * aspect ratio accepts 16:9 or 1280:720 forms. The callback is skipped on errors such as a wrong
   * size or when guest output is disabled. It must submit all update work to the host GPU before
   * returning successfully.
   *
   * @param frontbuffer_width Front buffer width in pixels.
   * @param frontbuffer_height Front buffer height in pixels.
   * @param display_aspect_ratio_x Display aspect ratio numerator.
   * @param display_aspect_ratio_y Display aspect ratio denominator.
   * @param refresher Callback that updates the image.
   * @return True when the callback was called and returned true.
   */
  bool RefreshGuestOutput(
      u32 frontbuffer_width, u32 frontbuffer_height, u32 display_aspect_ratio_x,
      u32 display_aspect_ratio_y,
      rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher);

  void AddOverlayLayer(gpu::IOverlayLayer* layer, size_t z_order) override;
  void RemoveOverlayLayer(gpu::IOverlayLayer* layer) override;

  /** Requests repainting with the UI when there is UI to draw. */
  void RequestOverlayPaint() override;

 protected:
  /**
   * Sets the host and surface from the Window the presenter is attached to.
   *
   * May run from the presenter destructor through the window.
   *
   * @param new_host Presentation host.
   * @param new_surface Surface to paint to.
   */
  void SetWindowSurfaceFromUIThread(gpu::IPresentationHost* new_host, gpu::Surface* new_surface);
  void OnSurfaceMonitorUpdateFromUIThread(bool old_monitor_potentially_disconnected);
  void OnSurfaceResizeFromUIThread();

  /**
   * Paints from the platform paint event handler.
   *
   * Without UI overlays, and with the guest output presented from the refreshing thread, the
   * paint is skipped unless requested earlier or force_paint is true. A paint draws over a black
   * background and letterbox, so windows should have a black background before a Presenter is
   * attached.
   *
   * @param force_paint True to paint even when no paint was requested.
   */
  void PaintFromUIThread(bool force_paint = false);

  enum class PaintResult : u8 {
    kPresented,
    kPresentedSuboptimal,
    /** Refused for internal reasons or a host API failure, presenting may retry without a reset. */
    kNotPresented,
    kNotPresentedConnectionOutdated,
    kGPULostExternally,
    kGPULostResponsible,
  };

  enum class SurfacePaintConnectResult : u8 {
    /** Nothing changed so no redraw is needed. Never returned for a new connection. */
    kSuccessUnchanged,
    kSuccess,
    kFailure,
    kFailureSurfaceUnusable,
  };

  static constexpr u32 kGuestOutputMailboxSize = 3;

  struct GuestOutputProperties {
    /** Any dimension or aspect ratio value being 0 disables the guest output for the frame. */
    u32 frontbuffer_width;
    u32 frontbuffer_height;
    /** Aspect ratio numerator and denominator, in 16:9 or 1280:720 form. */
    u32 display_aspect_ratio_x;
    u32 display_aspect_ratio_y;
    bool is_8bpc;

    GuestOutputProperties() { SetToInactive(); }

    bool IsActive() const {
      return frontbuffer_width && frontbuffer_height && display_aspect_ratio_x &&
             display_aspect_ratio_y;
    }

    void SetToInactive() {
      frontbuffer_width = 0;
      frontbuffer_height = 0;
      display_aspect_ratio_x = 0;
      display_aspect_ratio_y = 0;
      is_8bpc = false;
    }
  };

  enum class GuestOutputPaintEffect : u8 {
    kBilinear,
    kBilinearDither,

    kCount,
  };

  struct GuestOutputPaintFlow {
    static constexpr size_t kMaxClearRectangles = 4;

    struct ClearRectangle {
      u32 x;
      u32 y;
      u32 width;
      u32 height;
    };

    GuestOutputProperties properties;

    /** False means the guest output is not displayed. */
    bool has_effect;
    GuestOutputPaintEffect effect;
    std::pair<u32, u32> effect_output_size;

    /** Offset of the final draw rectangle in the host window, with letterboxing. */
    i32 output_x;
    i32 output_y;

    /** Letterbox rectangles around the guest output, valid when has_effect is true. */
    size_t letterbox_clear_rectangle_count;
    std::array<ClearRectangle, kMaxClearRectangles> letterbox_clear_rectangles;
  };

  struct BilinearConstants {
    i32 output_offset[2];
    f32 output_size_inv[2];

    void Initialize(const GuestOutputPaintFlow& flow) {
      output_offset[0] = flow.output_x;
      output_offset[1] = flow.output_y;
      const std::pair<u32, u32>& output_size = flow.effect_output_size;
      output_size_inv[0] = 1.0f / static_cast<f32>(output_size.first);
      output_size_inv[1] = 1.0f / static_cast<f32>(output_size.second);
    }
  };

  explicit Presenter(HostGPULossCallback host_gpu_loss_callback,
                     const XenosGraphicsDriverBase& driver)
      : host_gpu_loss_callback_(std::move(host_gpu_loss_callback)), driver_(driver) {}

  /** Finishes the initialization shared by all drivers. */
  bool InitializeCommonSurfaceIndependent();

  /**
   * Callable only by the UI thread while it has painting access (PaintMode is not
   * kGuestOutputThreadImmediately) and only for a nonzero area surface that may support painting.
   * On failure the implementation must hold no resources referencing the surface. On success it
   * must report whether the host window system now forces vertical sync, which may cause CPU waits
   * when beginning or ending frames.
   *
   * @param new_surface_width Surface width in pixels.
   * @param new_surface_height Surface height in pixels.
   * @param was_paintable True when the previous connection was paintable.
   * @param is_vsync_implicit_out Receives whether the host forces vertical sync.
   * @return The connection result.
   */
  virtual SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      gpu::Surface& new_surface, u32 new_surface_width, u32 new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) = 0;
  /**
   * Releases resources referencing the surface held by the implementation.
   *
   * Invoked by DisconnectPaintingFromSurfaceFromUIThread only while the connection is active.
   */
  virtual void DriverDisconnectPaintingFromSurface() = 0;

  /**
   * Acquires the ready guest output image for a consumer.
   *
   * The returned lock interlocks consumers but not the producer, and must be held while accessing
   * implementation objects that depend on the image or its mailbox index, unless other locking
   * exists. While held, the acquired image cannot be released, made writable or given to another
   * consumer. Properties are returned by copy so the lock can be released earlier.
   *
   * @param mailbox_index_or_max_if_inactive_out Receives the mailbox index or UINT32_MAX.
   * @param properties_out Receives a copy of the image properties.
   * @param paint_config_out Receives a copy of the paint configuration.
   * @return The consumer lock, with an inactive image reported as UINT32_MAX.
   */
  [[nodiscard]] std::unique_lock<std::mutex> ConsumeGuestOutput(
      u32& mailbox_index_or_max_if_inactive_out, GuestOutputProperties* properties_out,
      GuestOutputPaintConfig* paint_config_out);
  /**
   * Computes how the guest output is painted to the host render target.
   *
   * Properties and configuration are passed in so copies can be used outside the consumer lock.
   *
   * @param host_rt_width Host render target width in pixels.
   * @param host_rt_height Host render target height in pixels.
   * @param max_rt_width Maximum render target width in pixels.
   * @param max_rt_height Maximum render target height in pixels.
   */
  GuestOutputPaintFlow GetGuestOutputPaintFlow(const GuestOutputProperties& properties,
                                               u32 host_rt_width, u32 host_rt_height,
                                               u32 max_rt_width, u32 max_rt_height,
                                               const GuestOutputPaintConfig& config) const;
  /**
   * Refreshes the guest output image of a mailbox slot through the refresher.
   *
   * The 8bpc result stays false when the refresher does not write it.
   *
   * @param mailbox_index Mailbox slot to write.
   * @param frontbuffer_width Front buffer width in pixels.
   * @param frontbuffer_height Front buffer height in pixels.
   * @param refresher Callback that updates the image.
   * @param is_8bpc_out_ref Receives whether the source has at most 8 bits per channel.
   * @return True when the refresher succeeded.
   */
  virtual bool DriverRefreshGuestOutput(
      u32 mailbox_index, u32 frontbuffer_width, u32 frontbuffer_height,
      rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) = 0;

  /**
   * Paints and presents the guest output, or solid black, with the UI on top when requested.
   *
   * May run on a non UI thread only to paint the guest output with execute_ui_drawers false.
   *
   * @param execute_ui_drawers True to draw the UI overlays.
   */
  virtual PaintResult DriverPaintAndPresent(bool execute_ui_drawers) = 0;

  /** Executes the UI drawers on behalf of the painting implementations. */
  void ExecuteOverlayLayersFromUIThread(gpu::OverlayDrawContext& ui_draw_context);

  /**
   * The vsync value the current paint connection was made with. Connection and painting use it
   * instead of the cvar, so both keep the same value until the next reconnection.
   */
  bool GetPaintConnectionVsync() const { return surface_paint_connection_vsync_; }

 private:
  enum class PaintMode : u8 {
    /**
     * Does not paint, and the painting lifecycle is UI thread only.
     *
     * host_->RequestPaint() must never be called since the Window may lack a paintable Surface
     * (closed or mid change), and non UI threads could race inside Window::RequestPaint on the
     * Window state. The only valid mode when there is no Surface.
     */
    kNone,
    /**
     * Guest output refreshing notifies host_, valid for RequestPaint, to paint in the UI thread.
     *
     * Painting, including the UI, and the painting lifecycle are UI thread only.
     */
    kUIThreadOnRequest,
    /**
     * Paints immediately in the guest output thread for lower latency.
     *
     * host_ may be notified through RequestPaint that the connection is outdated, and the
     * connection state changes only to kConnectedOutdated. Painting is guest output thread only,
     * and the UI thread cannot manage the lifecycle until it takes over.
     */
    kGuestOutputThreadImmediately,
  };

  enum class SurfacePaintConnectionState : u8 {
    /**
     * No surface, or connecting failed for the current surface state such as a zero size from
     * minimizing, or an outdated connection failed to reconnect.
     *
     * Reconnects when the surface state changes, such as its size.
     */
    kUnconnectedRetryAtStateChange,
    /** The surface is lost or incompatible, so retrying is pointless until it is replaced. */
    kUnconnectedSurfaceReportedUnusable,
    /** Paintable, though the connection may have become suboptimal and not yet been refreshed. */
    kConnectedPaintable,
    /**
     * The implementation holds connection resources but presentation reported it outdated.
     *
     * Reconnects at the next paint attempt, requesting one if needed. The only state the guest
     * output thread may set, from kConnectedPaintable only, while the paint mode is
     * kGuestOutputThreadImmediately.
     */
    kConnectedOutdated,
  };

  static constexpr bool IsConnectedSurfacePaintConnectionState(
      SurfacePaintConnectionState connection_state) {
    return connection_state == SurfacePaintConnectionState::kConnectedPaintable ||
           connection_state == SurfacePaintConnectionState::kConnectedOutdated;
  }

  struct OverlayLayerReference {
    gpu::IOverlayLayer* layer;
    u64 last_draw;

    explicit OverlayLayerReference(gpu::IOverlayLayer* layer, u64 last_draw = UINT64_MAX)
        : layer(layer), last_draw(last_draw) {}
  };

  void SetPaintModeFromUIThread(PaintMode new_mode);
  /** Returns the paint mode for the UI drawing and implicit vertical sync state. */
  PaintMode GetDesiredPaintModeFromUIThread(bool is_paintable) const;

  /**
   * Connects to a surface after being unconnected, or handles surface changes such as resizing.
   *
   * UI thread only, while it has painting access. The connection state must be
   * kUnconnectedRetryAtStateChange when connecting, otherwise the call is ignored. Never switch
   * from one surface to another without disconnecting first.
   */
  void UpdateSurfacePaintConnectionFromUIThread(bool* repaint_needed_out,
                                                bool update_paint_mode_to_desired);

  /**
   * UI thread only, while it has painting access.
   *
   * @param new_state The connection state after the disconnect.
   */
  void DisconnectPaintingFromSurfaceFromUIThread(SurfacePaintConnectionState new_state);

  /**
   * Requests a paint, or connection recovery, through the window. Callable from any thread when
   * host_ is open and safe for RequestPaint, so the connection state is not checked.
   *
   * @return True when host_->RequestPaint() was called.
   */
  bool RequestPaintOrConnectionRecoveryViaWindow(bool force_ui_thread_paint_tick);

  /**
   * Refreshes the monitor of the current window surface.
   *
   * The monitor reference is held only while a Surface exists, so it is cleared when the Window
   * loses its Surface on closing while staying attached to the Presenter.
   */
  void UpdateSurfaceMonitorFromUIThread(bool old_monitor_potentially_disconnected);

  /**
   * Reports whether the attached surface is visible on any monitor.
   *
   * UI thread painting may be skipped when false, so painting must be requested when the surface
   * reappears on a monitor.
   */
  bool IsSurfaceOnMonitorFromUIThread() const;

  /**
   * Calls DriverPaintAndPresent and runs post paint checks safe on both the UI and guest output
   * threads.
   *
   * A kPresentedSuboptimal result is returned as is. The image reached OS presentation, unlike
   * kNotPresentedConnectionOutdated.
   */
  PaintResult PaintAndPresent(bool execute_ui_drawers);

  void HandleUIDrawersChangeFromUIThread(bool drawers_were_empty);

  /**
   * Reports whether UI ticks are needed.
   *
   * Requires UI drawing and possible painting. Implicit vertical sync rules it out, as limiting
   * the frame rate too would cause inconsistent pacing and skipped vertical sync intervals.
   */
  bool IsUITickNeededFromUIThread() const {
    return !overlay_layers_.empty() && paint_mode_ != PaintMode::kNone &&
           !surface_paint_connection_has_implicit_vsync_;
  }
  void UpdateUITicksNeededFromUIThread();
  void WaitForUITickFromUIThread();
  /** May be called from any thread. */
  void ForceUIThreadPaintTick();

  /**
   * Handler for host GPU loss, called only at the end of entry points.
   *
   * It may reinitialize the presenter when called from the UI thread.
   */
  HostGPULossCallback host_gpu_loss_callback_;

  const XenosGraphicsDriverBase& driver_;

  /**
   * Used by the guest output thread when the paint mode is not kNone, to request painting
   * (kUIThreadOnRequest) or reconnection (kGuestOutputThreadImmediately) in the UI thread.
   *
   * The paint mode must be kNone before modifying it.
   */
  gpu::IPresentationHost* host_ = nullptr;

  gpu::Surface* surface_ = nullptr;

  /**
   * Protects paint_mode_ and, in the guest output thread, the painting objects.
   *
   * The UI thread locks it to disable guest output thread painting access, works on the painting
   * connection, then locks it to re enable the access. The guest output thread must hold it
   * throughout painting.
   */
  std::mutex paint_mode_mutex_;
  /** Writable by the UI thread, read only for the guest output thread. */
  PaintMode paint_mode_ = PaintMode::kNone;

  /**
   * Accessed only by the guest output thread in kGuestOutputThreadImmediately, otherwise only by
   * the UI thread.
   *
   * The guest output thread may change the connection state, only from kConnectedPaintable to
   * kConnectedOutdated.
   */
  SurfacePaintConnectionState surface_paint_connection_state_ =
      SurfacePaintConnectionState::kUnconnectedRetryAtStateChange;
  /** Reconnects only when a connection optimal at the last paint became suboptimal. */
  bool surface_paint_connection_was_optimal_at_successful_paint_ = false;

  /** Modifiable only by the UI thread, readable by it in any paint mode, while connecting. */
  bool surface_paint_connection_has_implicit_vsync_ = false;
  /** Modifiable only by the UI thread, readable by the painting thread. */
  u32 surface_width_in_paint_connection_ = 0;
  u32 surface_height_in_paint_connection_ = 0;

  /**
   * Set by the UI or guest output thread before host_->RequestPaint() when an extra paint,
   * preceded by reconnection if needed, was requested after a state change that may affect the
   * painting connection.
   *
   * It needs no reset on connection loss because the next successful reconnection is followed by
   * a repaint request.
   */
  std::atomic<bool> ui_thread_paint_requested_{false};

  /** Spaces overlay only paints and wakes the UI thread when a guest frame is published. */
  OverlayPaintPacer overlay_paint_pacer_;

  /** Modifiable only by the UI thread while connecting, readable by the painting thread. */
  bool surface_paint_connection_vsync_ = true;

  /** UI thread only. Start of the previous paint, which spaces overlay only paints. */
  std::chrono::steady_clock::time_point last_ui_thread_paint_start_{};

  std::mutex guest_output_paint_config_mutex_;
  /** Writable by the UI thread, read only for the guest output thread. */
  GuestOutputPaintConfig guest_output_paint_config_;

  static_assert(kGuestOutputMailboxSize == 3);
  /**
   * Lock free single producer mailbox for presenting the latest guest output image, with
   * consumers serialized by a lock.
   *
   * The acquired image (bits 0:1) is being or was last read by a consumer, and only the consumer
   * changes its index. The ready image (bits 2:3) is the newest written image a consumer may
   * acquire, equal to the acquired image when no refresh happened since the last acquisition.
   */
  std::atomic<u32> guest_output_mailbox_acquired_and_ready_{0};
  /** Differs from the acquired and ready images and is written only by guest output refreshing. */
  u32 guest_output_mailbox_writable_ = 1;
  /**
   * Serializes consumers of the guest output images, painting and CPU buffer capture.
   *
   * It must not be held while locking paint_mode_mutex_. The guest output thread paints with
   * paint_mode_mutex_ held and consumes as part of painting, so the lock order there is the
   * reverse.
   */
  std::mutex guest_output_mailbox_consumer_mutex_;

  std::array<GuestOutputProperties, kGuestOutputMailboxSize> guest_output_properties_;
  /** Accessed only by refreshing, true when the last refresh contained an image. */
  bool guest_output_active_last_refresh_ = false;

  /**
   * Ordered by Z order, then by time of addition.
   *
   * Iteration over this order must match input handling in the Window input listeners, in
   * reverse.
   */
  std::multimap<size_t, OverlayLayerReference> overlay_layers_;

  size_t ui_draw_current_ = 0;
  size_t ui_draw_current_z_order_;
  std::multimap<size_t, OverlayLayerReference>::iterator ui_draw_next_iterator_;
  bool is_executing_ui_drawers_ = false;

  /** True inside PaintFromUIThread, so paint mode changes and redraw requests defer to its tail. */
  bool is_in_ui_thread_paint_ = false;
  bool request_guest_output_paint_after_current_ui_thread_paint_;
  bool request_ui_paint_after_current_ui_thread_paint_;

  // Waits here must stay interruptible by guest output presentation requests to bound latency.
#if REX_PLATFORM_WIN32
  static Microsoft::WRL::ComPtr<IDXGIOutput> GetDXGIOutputForMonitor(IDXGIFactory1* factory,
                                                                     HMONITOR monitor);
  bool IsDXGIUITickWaitable(
      [[maybe_unused]] const std::unique_lock<std::mutex>& dxgi_ui_tick_lock) {
    return dxgi_ui_ticks_needed_ && !dxgi_ui_tick_thread_shutdown_ && dxgi_ui_tick_output_;
  }
  void DXGIUITickThread();

  /** UI thread only, so monitor dependent data such as the DXGI output updates only on a change. */
  HMONITOR surface_win32_monitor_ = nullptr;

  /**
   * Created on demand with DXGI 1.1, needed by IDXGIFactory1::IsCurrent (Windows 7 and later).
   * Mixing DXGI 1.0 and 1.1 or later in the Direct3D 12 code is unsupported. It may be released
   * and recreated once it is no longer current.
   */
  Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_ui_tick_factory_;

  /** UI thread only, though the value comes from the tick mutex protected variable. */
  u64 dxgi_ui_tick_last_draw_ = 0;

  std::mutex dxgi_ui_tick_mutex_;
  u64 dxgi_ui_tick_last_vblank_ = 1;
  /**
   * The signal may not be sent when the output is null or shutdown is set. The frame rate is then
   * not limited, as in a failure to find the output, or nothing is drawn when the window left a
   * connected monitor.
   */
  Microsoft::WRL::ComPtr<IDXGIOutput> dxgi_ui_tick_output_;
  /**
   * The thread waits for vertical blanking only while UI ticks are needed, otherwise it sleeps on
   * the control condition variable. Modifiable only from the UI thread.
   */
  bool dxgi_ui_ticks_needed_ = false;
  /** Modifiable only from the UI thread. */
  bool dxgi_ui_tick_thread_shutdown_ = false;
  bool dxgi_ui_tick_force_requested_ = false;

  std::condition_variable dxgi_ui_tick_control_condition_;
  /** May be signaled by guest output refreshing. */
  std::condition_variable dxgi_ui_tick_signal_condition_;

  std::jthread dxgi_ui_tick_thread_;
#endif  // REX_PLATFORM_WIN32
};

}  // namespace gpu
}  // namespace rex
