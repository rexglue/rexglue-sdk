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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <utility>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/ui/window.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>
#if REX_PLATFORM_WIN32
#include <rex/utility.h>
#endif
#include "gpu/xenos/host/present_policy.h"
#include "gpu/xenos/host/presenter.h"

#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace {
using GuestOutputPaintConfig = rex::gpu::Presenter::GuestOutputPaintConfig;

GuestOutputPaintConfig BuildGuestOutputPaintConfig(const rex::gpu::XenosSettings& settings) {
  GuestOutputPaintConfig config;
  config.SetAllowOverscanCutoff(settings.present_allow_overscan_cutoff.Get());
  config.SetDither(settings.present_dither.Get());
  return config;
}

}  // namespace

namespace rex {
namespace gpu {

void Presenter::FatalErrorHostGPULossCallback([[maybe_unused]] bool is_responsible,
                                              [[maybe_unused]] bool statically_from_ui_thread) {
  REX_FATAL(gpu, "Graphics device lost (probably due to an internal error)");
}

Presenter::~Presenter() {
  REX_ASSERT(!is_executing_ui_drawers_);

#if REX_PLATFORM_WIN32
  if (dxgi_ui_tick_thread_.joinable()) {
    {
      std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      dxgi_ui_tick_thread_shutdown_ = true;
    }
    dxgi_ui_tick_control_condition_.notify_all();
    dxgi_ui_tick_thread_.join();
  }
#endif  // REX_PLATFORM_WIN32

  if (host_) {
    gpu::IPresentationHost* old_host = host_;
    host_ = nullptr;
    old_host->OnPresenterDetached();
  }
}

void Presenter::SetWindowSurfaceFromUIThread(gpu::IPresentationHost* new_host,
                                             gpu::Surface* new_surface) {
  REX_ASSERT(!is_executing_ui_drawers_);

  REX_ASSERT(!(new_surface && !new_host));

  if (host_ == new_host && (!host_ || surface_ == new_surface)) {
    return;
  }

  if (surface_) {
    SetPaintModeFromUIThread(PaintMode::kNone);
    DisconnectPaintingFromSurfaceFromUIThread(
        SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    surface_ = nullptr;
    UpdateSurfaceMonitorFromUIThread(true);
  }

  if (host_ != new_host) {
    if (host_) {
      gpu::IPresentationHost* old_host = host_;
      host_ = nullptr;
      old_host->OnPresenterDetached();
    }

    host_ = new_host;
  }

  if (new_surface) {
    REX_ASSERT(paint_mode_ == PaintMode::kNone);
    surface_ = new_surface;
    UpdateSurfaceMonitorFromUIThread(true);
    REX_ASSERT(surface_paint_connection_state_ ==
               SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    bool request_repaint;
    UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);
    if (request_repaint) {
      RequestPaintOrConnectionRecoveryViaWindow(true);
    }
  }
}

void Presenter::OnSurfaceMonitorUpdateFromUIThread(bool old_monitor_potentially_disconnected) {
  REX_ASSERT(!is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  UpdateSurfaceMonitorFromUIThread(old_monitor_potentially_disconnected);
}

void Presenter::OnSurfaceResizeFromUIThread() {
  REX_ASSERT(!is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  // Downgrade to kUIThreadOnRequest, not kNone, so frames arriving mid resize are not missed.
  if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
    SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
  }

  bool request_repaint;
  UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);

  if (request_repaint) {
    RequestPaintOrConnectionRecoveryViaWindow(true);
  }
}

void Presenter::PaintFromUIThread(bool force_paint) {
  // The monitor check is explicit because UI frame rate limiting may be tied to OS monitor signals.
  if (!IsSurfaceOnMonitorFromUIThread()) {
    return;
  }

  REX_ASSERT(!is_in_ui_thread_paint_);
  is_in_ui_thread_paint_ = true;
  request_guest_output_paint_after_current_ui_thread_paint_ = false;
  request_ui_paint_after_current_ui_thread_paint_ = false;

  bool draw_ui = !overlay_layers_.empty();
  bool do_paint = force_paint || draw_ui;
  if (ui_thread_paint_requested_.exchange(false, std::memory_order_relaxed)) {
    do_paint = true;
  }
  PaintResult paint_result = PaintResult::kNotPresented;
  bool request_repaint_at_tick = false;
  bool request_repaint_immediately = false;
  if (do_paint) {
    // Downgrade to kUIThreadOnRequest, not kNone, so frames generated mid paint notify.
    if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
    }
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable &&
        IsPaintConnectionVsyncStale(surface_paint_connection_vsync_, driver_.IsVSyncEnabled())) {
      surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
    }
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
      UpdateSurfacePaintConnectionFromUIThread(nullptr, false);
    }
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);

      overlay_paint_pacer_.Wait(last_ui_thread_paint_start_, force_paint);
      last_ui_thread_paint_start_ = std::chrono::steady_clock::now();

      WaitForUITickFromUIThread();

      paint_result = PaintAndPresent(draw_ui);
      if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
        request_repaint_immediately = true;
      }
    }
    if (surface_paint_connection_state_ != SurfacePaintConnectionState::kConnectedPaintable) {
      SetPaintModeFromUIThread(PaintMode::kNone);
    }
  }

  // The connection state may be owned by the guest output thread here, so check only the mode.
  if (paint_mode_ != PaintMode::kNone) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
  }
  is_in_ui_thread_paint_ = false;

  if (paint_result == PaintResult::kGPULostExternally ||
      paint_result == PaintResult::kGPULostResponsible) {
    if (host_gpu_loss_callback_) {
      host_gpu_loss_callback_(paint_result == PaintResult::kGPULostResponsible, true);
    }
    // The loss callback may have destroyed the presenter, so never touch this afterwards.
    return;
  }

  if (paint_mode_ != PaintMode::kNone) {
    if (request_guest_output_paint_after_current_ui_thread_paint_ ||
        (draw_ui && overlay_layers_.empty())) {
      request_repaint_immediately = true;
    }
    if (request_ui_paint_after_current_ui_thread_paint_ && !overlay_layers_.empty()) {
      request_repaint_at_tick = true;
    }
  }
  if (request_repaint_at_tick || request_repaint_immediately) {
    RequestPaintOrConnectionRecoveryViaWindow(request_repaint_immediately);
  }
}

bool Presenter::RefreshGuestOutput(
    u32 frontbuffer_width, u32 frontbuffer_height, u32 display_aspect_ratio_x,
    u32 display_aspect_ratio_y,
    rex::MoveOnlyFunction<bool(GuestOutputRefreshContext& context)> refresher) {
  GuestOutputProperties& writable_properties =
      guest_output_properties_[guest_output_mailbox_writable_];
  writable_properties.frontbuffer_width = frontbuffer_width;
  writable_properties.frontbuffer_height = frontbuffer_height;
  writable_properties.display_aspect_ratio_x = display_aspect_ratio_x;
  writable_properties.display_aspect_ratio_y = display_aspect_ratio_y;
  writable_properties.is_8bpc = false;
  bool is_active = writable_properties.IsActive();
  if (is_active) {
    if (!DriverRefreshGuestOutput(guest_output_mailbox_writable_, frontbuffer_width,
                                  frontbuffer_height, std::move(refresher),
                                  writable_properties.is_8bpc)) {
      return false;
    }
    guest_output_active_last_refresh_ = true;
  } else {
    if (!guest_output_active_last_refresh_) {
      return false;
    }
    guest_output_active_last_refresh_ = false;
  }

  // Replaces the pending ready image rather than queueing behind it, for the lowest latency.
  u32 last_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  while (!guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
      last_acquired_and_ready,
      (last_acquired_and_ready & 3) | (guest_output_mailbox_writable_ << 2),
      std::memory_order_acq_rel, std::memory_order_relaxed)) {}
  u32 last_acquired = last_acquired_and_ready & 3;
  if (last_acquired == guest_output_mailbox_writable_) {
    // Not (3 - a - b) % 3, which yields a when a == b. Using + 1 makes waits target 3 frames ago.
    guest_output_mailbox_writable_ = (guest_output_mailbox_writable_ + 1) % 3;
  } else {
    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;
  }

  overlay_paint_pacer_.SignalGuestOutput();
  PaintResult paint_result = PaintResult::kNotPresented;
  {
    std::lock_guard<std::mutex> paint_mode_mutex_lock(paint_mode_mutex_);
    switch (paint_mode_) {
      case PaintMode::kNone:
        break;
      case PaintMode::kUIThreadOnRequest:
        RequestPaintOrConnectionRecoveryViaWindow(true);
        break;
      case PaintMode::kGuestOutputThreadImmediately:
        if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
          paint_result = PaintAndPresent(false);
          if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
            RequestPaintOrConnectionRecoveryViaWindow(true);
          }
        }
        break;
    }
  }
  if (host_gpu_loss_callback_) {
    if (paint_result == PaintResult::kGPULostResponsible) {
      host_gpu_loss_callback_(true, false);
    } else if (paint_result == PaintResult::kGPULostExternally) {
      host_gpu_loss_callback_(false, false);
    }
  }

  return is_active;
}

void Presenter::AddOverlayLayer(gpu::IOverlayLayer* layer, size_t z_order) {
  REX_ASSERT(layer != nullptr);
  bool drawers_were_empty = overlay_layers_.empty();
  u64 drawer_last_draw = UINT64_MAX;
  for (auto it_existing = overlay_layers_.begin(); it_existing != overlay_layers_.end();
       ++it_existing) {
    if (it_existing->second.layer != layer) {
      continue;
    }
    if (it_existing->first == z_order) {
      return;
    }
    // Keep the last draw index so raising the Z order mid loop does not run the drawer twice.
    drawer_last_draw = it_existing->second.last_draw;
    // Skip the erased drawer if it is next in the loop, since a multimap invalidates only it.
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    overlay_layers_.erase(it_existing);
    break;
  }
  auto it_new = overlay_layers_.emplace(z_order, OverlayLayerReference(layer, drawer_last_draw));
  // Run a drawer added at or above the current Z order, but only up to the drawer that was next.
  if (is_executing_ui_drawers_ && z_order >= ui_draw_current_z_order_ &&
      (ui_draw_next_iterator_ == overlay_layers_.end() ||
       z_order < ui_draw_next_iterator_->first)) {
    ui_draw_next_iterator_ = it_new;
  }
  HandleUIDrawersChangeFromUIThread(drawers_were_empty);
}

void Presenter::RemoveOverlayLayer(gpu::IOverlayLayer* layer) {
  REX_ASSERT(layer != nullptr);
  for (auto it_existing = overlay_layers_.begin(); it_existing != overlay_layers_.end();
       ++it_existing) {
    if (it_existing->second.layer != layer) {
      continue;
    }
    // Skip the erased drawer if it is next in the loop, since a multimap invalidates only it.
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    overlay_layers_.erase(it_existing);
    HandleUIDrawersChangeFromUIThread(false);
    return;
  }
}

void Presenter::RequestOverlayPaint() {
  if (is_in_ui_thread_paint_) {
    request_ui_paint_after_current_ui_thread_paint_ = true;
    return;
  }
  // The guest output thread may own the connection state, so check coarsely via paint_mode_.
  if (!overlay_layers_.empty() && paint_mode_ != PaintMode::kNone) {
    host_->RequestPaint();
  }
}

bool Presenter::InitializeCommonSurfaceIndependent() {
  {
    std::lock_guard<std::mutex> config_lock(guest_output_paint_config_mutex_);
    guest_output_paint_config_ = BuildGuestOutputPaintConfig(driver_);
  }

#if REX_PLATFORM_WIN32
  dxgi_ui_tick_thread_ =
      platform::Spawn("DXGI UI Tick", [this](const std::stop_token&) { DXGIUITickThread(); });
#endif  // REX_PLATFORM_WIN32

  return true;
}

std::unique_lock<std::mutex> Presenter::ConsumeGuestOutput(
    u32& mailbox_index_or_max_if_inactive_out, GuestOutputProperties* properties_out,
    GuestOutputPaintConfig* paint_config_out) {
  if (paint_config_out) {
    std::unique_lock<std::mutex> config_lock(guest_output_paint_config_mutex_);
    *paint_config_out = guest_output_paint_config_;
  }

  std::unique_lock<std::mutex> consumer_lock(guest_output_mailbox_consumer_mutex_);
  u32 old_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  u32 desired_acquired_and_ready =
      (old_acquired_and_ready & ~static_cast<u32>(3)) | (old_acquired_and_ready >> 2);
  while (old_acquired_and_ready != desired_acquired_and_ready &&
         !guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
             old_acquired_and_ready, desired_acquired_and_ready, std::memory_order_acq_rel,
             std::memory_order_relaxed)) {
    desired_acquired_and_ready =
        (old_acquired_and_ready & ~static_cast<u32>(3)) | (old_acquired_and_ready >> 2);
  }
  u32 mailbox_index = desired_acquired_and_ready & 3;
  const GuestOutputProperties& properties = guest_output_properties_[mailbox_index];
  mailbox_index_or_max_if_inactive_out = properties.IsActive() ? mailbox_index : UINT32_MAX;
  if (properties_out) {
    *properties_out = properties;
  }
  return consumer_lock;
}

Presenter::GuestOutputPaintFlow Presenter::GetGuestOutputPaintFlow(
    const GuestOutputProperties& properties, u32 host_rt_width, u32 host_rt_height,
    u32 max_rt_width, u32 max_rt_height, const GuestOutputPaintConfig& config) const {
  GuestOutputPaintFlow flow = {};

  REX_ASSERT(max_rt_width != 0);
  REX_ASSERT(max_rt_height != 0);

  flow.letterbox_clear_rectangle_count = 1;
  flow.letterbox_clear_rectangles[0].width = host_rt_width;
  flow.letterbox_clear_rectangles[0].height = host_rt_height;

  if (!properties.IsActive() || !host_rt_width || !host_rt_height ||
      !surface_width_in_paint_connection_ || !surface_height_in_paint_connection_) {
    return flow;
  }

  flow.properties = properties;

  auto rescale_unsigned = [](u32 value, u32 new_scale, u32 old_scale) -> u32 {
    return static_cast<u32>((static_cast<u64>(value) * new_scale + (old_scale >> 1)) / old_scale);
  };
  auto rescale_signed = [](i32 value, u32 new_scale, u32 old_scale) -> i32 {
    // The sign of the half scale offset matches the value, since / truncates toward zero.
    return static_cast<i32>((static_cast<i64>(value) * new_scale +
                             static_cast<i64>(old_scale >> 1) * (value < 0 ? -1 : 1)) /
                            old_scale);
  };

  u32 output_width, output_height;
  if (static_cast<u64>(surface_width_in_paint_connection_) * properties.display_aspect_ratio_y >
      static_cast<u64>(surface_height_in_paint_connection_) * properties.display_aspect_ratio_x) {
    u32 present_safe_area;
    if (config.GetAllowOverscanCutoff() && driver_.present_safe_area_y.Get() > 0 &&
        driver_.present_safe_area_y.Get() < 100) {
      present_safe_area = static_cast<u32>(driver_.present_safe_area_y.Get());
    } else {
      present_safe_area = 100;
    }
    output_height =
        rescale_unsigned(surface_width_in_paint_connection_, properties.display_aspect_ratio_y,
                         properties.display_aspect_ratio_x);
    bool letterbox = false;
    if (output_height * present_safe_area > surface_height_in_paint_connection_ * 100) {
      output_height = rescale_unsigned(surface_height_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && driver_.present_letterbox.Get()) {
      output_width = rescale_unsigned(surface_height_in_paint_connection_ * 100,
                                      properties.display_aspect_ratio_x,
                                      properties.display_aspect_ratio_y * present_safe_area);
      flow.output_x =
          (static_cast<i32>(surface_width_in_paint_connection_) - static_cast<i32>(output_width)) /
          2;
    } else {
      output_width = surface_width_in_paint_connection_;
      flow.output_x = 0;
    }
    flow.output_y =
        (static_cast<i32>(surface_height_in_paint_connection_) - static_cast<i32>(output_height)) /
        2;
  } else {
    u32 present_safe_area;
    if (config.GetAllowOverscanCutoff() && driver_.present_safe_area_x.Get() > 0 &&
        driver_.present_safe_area_x.Get() < 100) {
      present_safe_area = static_cast<u32>(driver_.present_safe_area_x.Get());
    } else {
      present_safe_area = 100;
    }
    output_width =
        rescale_unsigned(surface_height_in_paint_connection_, properties.display_aspect_ratio_x,
                         properties.display_aspect_ratio_y);
    bool letterbox = false;
    if (output_width * present_safe_area > surface_width_in_paint_connection_ * 100) {
      output_width = rescale_unsigned(surface_width_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && driver_.present_letterbox.Get()) {
      output_height = rescale_unsigned(surface_width_in_paint_connection_ * 100,
                                       properties.display_aspect_ratio_y,
                                       properties.display_aspect_ratio_x * present_safe_area);
      flow.output_y = (static_cast<i32>(surface_height_in_paint_connection_) -
                       static_cast<i32>(output_height)) /
                      2;
    } else {
      output_height = surface_height_in_paint_connection_;
      flow.output_y = 0;
    }
    flow.output_x =
        (static_cast<i32>(surface_width_in_paint_connection_) - static_cast<i32>(output_width)) / 2;
  }

  // Converts from surface pixels, 1:1 with the physical display, to render target pixels.
  if (host_rt_width != surface_width_in_paint_connection_) {
    flow.output_x =
        rescale_signed(flow.output_x, host_rt_width, surface_width_in_paint_connection_);
    output_width =
        rescale_unsigned(output_width, host_rt_width, surface_width_in_paint_connection_);
  }
  if (host_rt_height != surface_height_in_paint_connection_) {
    flow.output_y =
        rescale_signed(flow.output_y, host_rt_height, surface_height_in_paint_connection_);
    output_height =
        rescale_unsigned(output_height, host_rt_height, surface_height_in_paint_connection_);
  }

  i32 output_right = flow.output_x + static_cast<i32>(output_width);
  i32 output_bottom = flow.output_y + static_cast<i32>(output_height);
  if (flow.output_x >= 0 && flow.output_y >= 0 && flow.output_x <= 1 && flow.output_y <= 1 &&
      output_width < host_rt_width && output_height < host_rt_height) {
    static std::atomic<bool> logged_top_left_expand = false;
    if (!logged_top_left_expand.exchange(true)) {
      REXLOG_WARN(ui,
                  "Presenter: expanding guest output from {}x{} to host render target "
                  "{}x{} to avoid top left only presentation",
                  output_width, output_height, host_rt_width, host_rt_height);
    }
    output_width = host_rt_width;
    output_height = host_rt_height;
    output_right = static_cast<i32>(output_width);
    output_bottom = static_cast<i32>(output_height);
  }
  if (!output_width || !output_height || output_right <= 0 || output_bottom <= 0 ||
      flow.output_x >= static_cast<i32>(host_rt_width) ||
      flow.output_y >= static_cast<i32>(host_rt_height)) {
    return flow;
  }

  flow.effect_output_size = std::make_pair(output_width, output_height);
  flow.effect = GuestOutputPaintEffect::kBilinear;
  flow.has_effect = true;

  if (config.GetDither()) {
    if (!properties.is_8bpc || output_width != properties.frontbuffer_width ||
        output_height != properties.frontbuffer_height) {
      flow.effect = GuestOutputPaintEffect::kBilinearDither;
    }
  }

  if (flow.has_effect) {
    flow.letterbox_clear_rectangle_count = 0;
    u32 letterbox_mid_top = static_cast<u32>(std::max(flow.output_y, static_cast<i32>(0)));
    if (letterbox_mid_top) {
      REX_ASSERT(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = 0;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = letterbox_mid_top;
    }
    u32 letterbox_mid_bottom = std::min(static_cast<u32>(output_bottom), host_rt_height);
    u32 letterbox_mid_height = letterbox_mid_bottom - letterbox_mid_top;
    if (flow.output_x > 0) {
      REX_ASSERT(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_left =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_left.x = 0;
      letterbox_clear_rectangle_left.y = letterbox_mid_top;
      letterbox_clear_rectangle_left.width = static_cast<u32>(flow.output_x);
      letterbox_clear_rectangle_left.height = letterbox_mid_height;
    }
    if (static_cast<u32>(output_right) < host_rt_width) {
      REX_ASSERT(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_right =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_right.x = static_cast<u32>(output_right);
      letterbox_clear_rectangle_right.y = letterbox_mid_top;
      letterbox_clear_rectangle_right.width = host_rt_width - static_cast<u32>(output_right);
      letterbox_clear_rectangle_right.height = letterbox_mid_height;
    }
    if (letterbox_mid_bottom < host_rt_height) {
      REX_ASSERT(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = letterbox_mid_bottom;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = host_rt_height - letterbox_mid_bottom;
    }
  }

  return flow;
}

void Presenter::ExecuteOverlayLayersFromUIThread(gpu::OverlayDrawContext& ui_draw_context) {
  REX_ASSERT(is_in_ui_thread_paint_);
  REX_ASSERT(!is_executing_ui_drawers_);
  ui_draw_next_iterator_ = overlay_layers_.begin();
  is_executing_ui_drawers_ = true;
  while (ui_draw_next_iterator_ != overlay_layers_.end()) {
    // A drawer may modify the list, so ui_draw_next_iterator_ is not std::next of its prior value.
    auto it_current = ui_draw_next_iterator_++;
    if (it_current->second.last_draw != ui_draw_current_) {
      ui_draw_current_z_order_ = it_current->first;
      it_current->second.last_draw = ui_draw_current_;
      it_current->second.layer->Draw(ui_draw_context);
    }
  }
  is_executing_ui_drawers_ = false;
  ++ui_draw_current_;
}

void Presenter::SetPaintModeFromUIThread(PaintMode new_mode) {
  // Only the UI thread modifies the mode, so comparing without the lock is safe.
  if (paint_mode_ == new_mode) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(paint_mode_mutex_);
    paint_mode_ = new_mode;
  }
  UpdateUITicksNeededFromUIThread();
}

Presenter::PaintMode Presenter::GetDesiredPaintModeFromUIThread(bool is_paintable) const {
  if (!is_paintable) {
    return PaintMode::kNone;
  }
  if (surface_paint_connection_has_implicit_vsync_) {
    // Avoids host vertical sync CPU waits in the thread generating the guest output.
    return PaintMode::kUIThreadOnRequest;
  }
  if (!overlay_layers_.empty()) {
    return PaintMode::kUIThreadOnRequest;
  }
  return PaintMode::kGuestOutputThreadImmediately;
}

void Presenter::DisconnectPaintingFromSurfaceFromUIThread(SurfacePaintConnectionState new_state) {
  REX_ASSERT(!IsConnectedSurfacePaintConnectionState(new_state));
  if (IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_)) {
    DriverDisconnectPaintingFromSurface();
  }
  surface_paint_connection_state_ = new_state;
  surface_paint_connection_has_implicit_vsync_ = false;
  surface_width_in_paint_connection_ = 0;
  surface_height_in_paint_connection_ = 0;
}

void Presenter::UpdateSurfacePaintConnectionFromUIThread(bool* repaint_needed_out,
                                                         bool update_paint_mode_to_desired) {
  REX_ASSERT(surface_ != nullptr);

  REX_ASSERT(paint_mode_ != PaintMode::kGuestOutputThreadImmediately);

  if (repaint_needed_out) {
    *repaint_needed_out = false;
  }

  if (surface_paint_connection_state_ !=
      SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable) {
    u32 surface_width = 0, surface_height = 0;
    bool surface_area_available = surface_->GetSize(surface_width, surface_height);
    if (!surface_area_available) {
      DisconnectPaintingFromSurfaceFromUIThread(
          SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    } else {
      bool is_reconnect = IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_);
      bool is_vsync_implicit = false;
      surface_paint_connection_vsync_ = driver_.IsVSyncEnabled();
      SurfacePaintConnectResult connect_result = ConnectOrReconnectPaintingToSurfaceFromUIThread(
          *surface_, surface_width, surface_height,
          surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable,
          is_vsync_implicit);
      switch (connect_result) {
        case SurfacePaintConnectResult::kSuccess:
          if (repaint_needed_out) {
            *repaint_needed_out = true;
          }
          [[fallthrough]];
        case SurfacePaintConnectResult::kSuccessUnchanged:
          surface_paint_connection_was_optimal_at_successful_paint_ = false;
          surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedPaintable;
          surface_paint_connection_has_implicit_vsync_ = is_vsync_implicit;
          surface_width_in_paint_connection_ = surface_width;
          surface_height_in_paint_connection_ = surface_height;
          if (!is_reconnect) {
            *repaint_needed_out = true;
          }
          break;
        case SurfacePaintConnectResult::kFailure:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedRetryAtStateChange;
          break;
        case SurfacePaintConnectResult::kFailureSurfaceUnusable:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable;
          break;
      }
    }
  }

  if (update_paint_mode_to_desired) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(
        surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable));
  }
}

bool Presenter::RequestPaintOrConnectionRecoveryViaWindow(bool force_ui_thread_paint_tick) {
  REX_ASSERT(host_ != nullptr);
  REX_ASSERT(surface_ != nullptr);
  if (ui_thread_paint_requested_.exchange(true, std::memory_order_relaxed)) {
    return false;
  }
  if (force_ui_thread_paint_tick) {
    ForceUIThreadPaintTick();
  }
  host_->RequestPaint();
  return true;
}

void Presenter::UpdateSurfaceMonitorFromUIThread(
    [[maybe_unused]] bool old_monitor_potentially_disconnected) {
  // A non null monitor requires surface_ (implying host_), so closing a window clears it.
#if REX_PLATFORM_WIN32
  HMONITOR surface_new_win32_monitor = nullptr;
  if (surface_ && surface_->GetType() == gpu::Surface::kTypeIndexWin32HWND) {
    HWND hwnd = static_cast<HWND>(static_cast<const gpu::NativeSurface*>(surface_)->GetHWND());
    if (hwnd) {
      surface_new_win32_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    }
  }
  if (old_monitor_potentially_disconnected || surface_win32_monitor_ != surface_new_win32_monitor) {
    surface_win32_monitor_ = surface_new_win32_monitor;
    if (dxgi_ui_tick_factory_ && !dxgi_ui_tick_factory_->IsCurrent()) {
      // A newly connected monitor is absent from the old factory, so recreate it.
      {
        Microsoft::WRL::ComPtr<IDXGIOutput> old_factory_output_to_release;
        {
          std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
          old_factory_output_to_release = std::move(dxgi_ui_tick_output_);
        }
      }
      dxgi_ui_tick_factory_.Reset();
    }
    if (!dxgi_ui_tick_factory_) {
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_ui_tick_factory_)))) {
        REXLOG_ERROR(ui, "Presenter: Failed to create a DXGI factory");
      }
    }
    Microsoft::WRL::ComPtr<IDXGIOutput> new_dxgi_output;
    if (dxgi_ui_tick_factory_ && surface_new_win32_monitor) {
      new_dxgi_output =
          GetDXGIOutputForMonitor(dxgi_ui_tick_factory_.Get(), surface_new_win32_monitor);
    }
    // Notify even when the old output was released early, as the tick thread may wait on null.
    bool signal_dxgi_ui_tick_control;
    {
      std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      bool dxgi_output_was_null = (dxgi_ui_tick_output_ == nullptr);
      dxgi_ui_tick_output_ = new_dxgi_output;
      signal_dxgi_ui_tick_control = dxgi_output_was_null && IsDXGIUITickWaitable(dxgi_ui_tick_lock);
    }
    if (signal_dxgi_ui_tick_control) {
      dxgi_ui_tick_control_condition_.notify_all();
    }
  }
#endif  // REX_PLATFORM_WIN32
}

bool Presenter::IsSurfaceOnMonitorFromUIThread() const {
  if (!surface_) {
    return false;
  }
#if REX_PLATFORM_WIN32
  return surface_win32_monitor_ != nullptr;
#else
  return true;
#endif  // REX_PLATFORM_WIN32
}

Presenter::PaintResult Presenter::PaintAndPresent(bool execute_ui_drawers) {
  REX_ASSERT(!(execute_ui_drawers && !is_in_ui_thread_paint_));
  REX_ASSERT(surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable);
  if (IsPaintConnectionVsyncStale(surface_paint_connection_vsync_, driver_.IsVSyncEnabled())) {
    surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  PaintResult result = DriverPaintAndPresent(execute_ui_drawers);
  switch (result) {
    case PaintResult::kPresented:
      surface_paint_connection_was_optimal_at_successful_paint_ = true;
      break;
    case PaintResult::kPresentedSuboptimal:
      if (surface_paint_connection_was_optimal_at_successful_paint_) {
        surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      }
      break;
    case PaintResult::kNotPresentedConnectionOutdated:
      surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      break;
    default:
      break;
  }
  return result;
}

void Presenter::HandleUIDrawersChangeFromUIThread(bool drawers_were_empty) {
  if (is_in_ui_thread_paint_) {
    // Defer so lifecycle changes do not happen during drawing.
    if (!overlay_layers_.empty()) {
      request_ui_paint_after_current_ui_thread_paint_ = true;
    }
    return;
  }

  if (paint_mode_ == PaintMode::kNone) {
    return;
  }

  if (overlay_layers_.empty() != drawers_were_empty) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
    UpdateUITicksNeededFromUIThread();
  }

  // Disabling the UI needs a paint request, to draw a frame without the UI promptly.
  ForceUIThreadPaintTick();
  host_->RequestPaint();
}

void Presenter::UpdateUITicksNeededFromUIThread() {
#if REX_PLATFORM_WIN32
  bool new_needed = IsUITickNeededFromUIThread();
  if (dxgi_ui_ticks_needed_ == new_needed) {
    return;
  }
  bool signal_dxgi_ui_tick_control;
  {
    std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
    dxgi_ui_ticks_needed_ = new_needed;
    signal_dxgi_ui_tick_control = IsDXGIUITickWaitable(dxgi_ui_tick_lock);
  }
  if (signal_dxgi_ui_tick_control) {
    dxgi_ui_tick_control_condition_.notify_all();
  }
#endif
}

void Presenter::WaitForUITickFromUIThread() {
#if REX_PLATFORM_WIN32
  if (!IsUITickNeededFromUIThread()) {
    return;
  }
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  u64 last_vblank_before_wait = dxgi_ui_tick_last_vblank_;
  while (true) {
    if (dxgi_ui_tick_force_requested_) {
      dxgi_ui_tick_force_requested_ = false;
      return;
    }
    if (!IsDXGIUITickWaitable(dxgi_ui_tick_lock)) {
      return;
    }
    if (dxgi_ui_tick_last_vblank_ > dxgi_ui_tick_last_draw_) {
      dxgi_ui_tick_last_draw_ =
          std::min(last_vblank_before_wait + static_cast<u64>(1), dxgi_ui_tick_last_vblank_);
      return;
    }
    dxgi_ui_tick_signal_condition_.wait(dxgi_ui_tick_lock);
  }
#endif  // REX_PLATFORM_WIN32
}

void Presenter::ForceUIThreadPaintTick() {
#if REX_PLATFORM_WIN32
  std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  dxgi_ui_tick_force_requested_ = true;
#endif  // REX_PLATFORM_WIN32
}

#if REX_PLATFORM_WIN32
Microsoft::WRL::ComPtr<IDXGIOutput> Presenter::GetDXGIOutputForMonitor(IDXGIFactory1* factory,
                                                                       HMONITOR monitor) {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  for (UINT adapter_index = 0;
       SUCCEEDED(factory->EnumAdapters(adapter_index, adapter.ReleaseAndGetAddressOf()));
       ++adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT output_index = 0; SUCCEEDED(adapter->EnumOutputs(output_index, &output));
         ++output_index) {
      DXGI_OUTPUT_DESC output_desc;
      if (SUCCEEDED(output->GetDesc(&output_desc)) && output_desc.Monitor == monitor) {
        return output;
      }
    }
  }
  return nullptr;
}

void Presenter::DXGIUITickThread() {
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  while (true) {
    if (dxgi_ui_tick_thread_shutdown_) {
      return;
    }
    if (!IsDXGIUITickWaitable(dxgi_ui_tick_lock)) {
      dxgi_ui_tick_control_condition_.wait(dxgi_ui_tick_lock);
      continue;
    }
    // Wait unlocked while holding an output reference, so subscribers can early out.
    bool wait_succeeded;
    {
      Microsoft::WRL::ComPtr<IDXGIOutput> dxgi_output = dxgi_ui_tick_output_;
      dxgi_ui_tick_lock.unlock();
      wait_succeeded = SUCCEEDED(dxgi_ui_tick_output_->WaitForVBlank());
    }
    dxgi_ui_tick_lock.lock();
    if (wait_succeeded) {
      ++dxgi_ui_tick_last_vblank_;
    } else {
      dxgi_ui_tick_output_.Reset();
    }
    dxgi_ui_tick_signal_condition_.notify_all();
  }
}
#endif  // REX_PLATFORM_WIN32

}  // namespace gpu
}  // namespace rex
