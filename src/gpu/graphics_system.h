/**
 * @file        gpu/graphics_system.h
 * @brief       The graphics system the app owns
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <memory>

#include <rex/ui/overlay_renderer.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/graphics_system.h>
#include <rex/utility.h>

#include <rex/logging/log.h>

#include "gpu/graphics_control.h"

REXLOG_TABLE(kGraphicsLog, gpu)

namespace rex::gpu {

/** Runs the graphics driver and routes the Vd calls. The app starts and stops it. */
class GraphicsSystem final : public rex::gpu::IGraphicsSystem, public IGraphicsControl {
 public:
  GraphicsSystem();
  ~GraphicsSystem() override;

  rex::gpu::GraphicsSettings& GetSettings() override { return settings_; }
  bool HasDriver() const override;
  std::string GetDriverName() const override;
  void ReportFrame() override;
  rex::gpu::FrameStats GetFrameStats() const override;

  [[nodiscard]] Result<void> AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                         IPhysicalWriteWatch& watch,
                                         IGuestInterrupts& interrupts) override;
  void DetachTitle() override;
  void InitializeRingBuffer(u32 address, u32 size_log2) override;
  void EnableReadPointerWriteBack(u32 address, u32 block_size_log2) override;
  u32 ReadRegister(u32 index) override;
  void WriteRegister(u32 index, u32 value) override;
  void OnSurfaceResized(u32 width, u32 height) override;
  void SetUIPump(MoveOnlyFunction<bool(std::chrono::milliseconds)> serve) override;

  /**
   * Starts the present thread.
   *
   * @param graphics_driver The driver, or null to run without one.
   * @return Success, or the reason the system cannot start.
   */
  [[nodiscard]] Result<void> Start(rex::gpu::IGraphicsDriver* graphics_driver);

  void Stop();

  /**
   * Sets what draws the overlays in each present.
   *
   * @param hook Called on the present thread between BeginPresent and EndPresent.
   */
  void SetOverlayHook(MoveOnlyFunction<void(IOverlayRenderer&)> hook);

  /** Reports whether the driver is a Xenos driver that receives the Vd calls. */
  bool HasXenos() const;

 private:
  rex::gpu::GraphicsSettings settings_;
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::gpu
