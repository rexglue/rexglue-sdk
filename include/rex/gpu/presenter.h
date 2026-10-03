/**
 * @file        gpu/presenter.h
 * @brief       The presenter and its window side
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>

#include <rex/gpu/surface.h>

namespace rex::gpu {

class IOverlayLayer;

/** The window side of a presenter. */
class IPresentationHost {
 public:
  virtual ~IPresentationHost() = default;

  /** Requests a paint on the UI thread. Callable from any thread. */
  virtual void RequestPaint() = 0;

  /** The presenter left this host. */
  virtual void OnPresenterDetached() = 0;
};

/** Paints guest output and overlay layers into a surface. Every method runs on the UI thread. */
class IPresenter {
 public:
  virtual ~IPresenter() = default;

  /**
   * Returns the surface types this presenter paints into.
   *
   * @return The surface type flags.
   */
  virtual Surface::TypeFlags supported_surface_types() const = 0;

  /**
   * Attaches to a host and its surface. Attach(nullptr, nullptr) detaches.
   *
   * @param host The window side, or null.
   * @param surface The surface the host owns, or null.
   * @return True when the presenter can paint into the surface.
   */
  virtual bool Attach(IPresentationHost* host, Surface* surface) = 0;

  /**
   * Reacts to the surface moving to another monitor.
   *
   * @param old_monitor_potentially_disconnected True when the previous monitor may be gone.
   */
  virtual void OnSurfaceMonitorUpdate(bool old_monitor_potentially_disconnected) = 0;

  /** Reacts to a surface size change. */
  virtual void OnSurfaceResize() = 0;

  /**
   * Paints the guest output and the overlay layers.
   *
   * @param force True to paint even when nothing changed.
   */
  virtual void Paint(bool force) = 0;

  /** Requests a paint of the overlay layers. */
  virtual void RequestOverlayPaint() = 0;

  /**
   * Adds a layer, or moves it to a new z order.
   *
   * @param layer The layer.
   * @param z_order Higher draws later.
   */
  virtual void AddOverlayLayer(IOverlayLayer* layer, size_t z_order) = 0;

  /**
   * Removes a layer.
   *
   * @param layer The layer.
   */
  virtual void RemoveOverlayLayer(IOverlayLayer* layer) = 0;
};

}  // namespace rex::gpu
