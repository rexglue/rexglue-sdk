/**
 * @file        gpu/graphics_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/driver.h>
#include <rex/platform/native_window.h>
#include <rex/overlay_renderer.h>
#include <rex/guest.h>
#include <rex/memory/host.h>
#include <rex/result.h>
#include <rex/title.h>
#include <rex/types.h>

namespace rex::gpu {

/** The memory tag of graphics emulation, named "GPU". */
inline constexpr memory::Tag kGPU{3};

struct FrameStatus {
  bool new_frame = false; /**< The title finished a frame since the last call. */
};

class IXenosGraphicsDriver;

/**
 * A GPU that presents to the window. GraphicsSystem makes every call on its present thread. A
 * present is WaitForFrame, BeginPresent, the overlays through IOverlayRenderer, then EndPresent.
 */
class IGraphicsDriver : public IDriver, public IOverlayRenderer {
 public:
  /**
   * Casts to the Xenos driver. Works across a module boundary where dynamic_cast does not.
   *
   * @return The Xenos driver, or nullptr for a native driver.
   */
  virtual IXenosGraphicsDriver* AsXenos() { return nullptr; }

  /**
   * Gives the driver the window to present to. Called after Initialize and before the first
   * present, only when the app has a window.
   *
   * @return Ok, or the failure of the provider or presenter, which stops the graphics start.
   */
  [[nodiscard]] virtual Result<void> SetSurface(const NativeWindow& window) {
    (void)window;
    return {};
  }

  /**
   * Called when a title launches. The driver takes what it uses from the title here.
   *
   * @return Ok, or an error that stops the launch.
   */
  [[nodiscard]] virtual Result<void> AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                                 IPhysicalWriteWatch& watch,
                                                 IGuestInterrupts& interrupts) {
    (void)info;
    (void)memory;
    (void)watch;
    (void)interrupts;
    return {};
  }

  /**
   * The title is stopping. On return the driver holds no memory spans, listeners or threads.
   */
  virtual void DetachTitle() {}

  /** Waits until the title finishes a frame or the time runs out. */
  virtual FrameStatus WaitForFrame(u32 timeout_ms) = 0;

  /** Resizes the swap chain. Called before BeginPresent after the window changed size. */
  virtual void ResizeSurface(u32 width, u32 height) = 0;

  /**
   * Starts a present by drawing the latest title frame, scaled to the window, into the next swap
   * chain image.
   *
   * @return True when there is an image to draw to, false while minimized or the surface is lost,
   *         and the present is skipped.
   */
  [[nodiscard]] virtual bool BeginPresent() = 0;

  /** Finishes the present started by a BeginPresent that returned true. */
  virtual void EndPresent() = 0;
};

}  // namespace rex::gpu
