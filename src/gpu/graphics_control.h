/**
 * @file        gpu/graphics_control.h
 * @brief       The calls the title runtime and the UI system make on the graphics system
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

#include <rex/guest.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/title.h>
#include <rex/types.h>

namespace rex::gpu {

/** The graphics calls of the title runtime and the app. Not part of the public API. */
REX_DECLARE_PRIVATE_INTERFACE(IGraphicsControl, 1)
/**
 * @return Ok, or the error that stops the launch.
 */
  [[nodiscard]] virtual Result<void> AttachTitle(const TitleInfo& info, IPhysicalMemory& memory,
                                                 IPhysicalWriteWatch& watch,
                                                 IGuestInterrupts& interrupts) = 0;

  virtual void DetachTitle() = 0;

  /**
   * Sets up the command processor ring buffer. Without a Xenos driver it warns once and does
   * nothing.
   *
   * @param address Guest physical address of the ring buffer.
   * @param size_log2 Log2 of the ring buffer size.
   */
  virtual void InitializeRingBuffer(u32 address, u32 size_log2) = 0;

  /**
   * Turns on the read pointer write back. Without a Xenos driver it warns once and does nothing.
   *
   * @param address Guest physical address the read pointer is written to.
   * @param block_size_log2 Log2 of the write back block size.
   */
  virtual void EnableReadPointerWriteBack(u32 address, u32 block_size_log2) = 0;

  /**
   * Reads a GPU register for a guest MMIO read.
   *
   * @return The register value, and 0 for a register nothing backs.
   */
  virtual u32 ReadRegister(u32 index) = 0;

  /** Writes a GPU register for a guest MMIO write. */
  virtual void WriteRegister(u32 index, u32 value) = 0;

  /**
   * Notifies the present thread that the window changed size. The driver resizes before the next
   * BeginPresent.
   */
  virtual void OnSurfaceResized(u32 width, u32 height) = 0;

  /**
   * Sets the function that runs posted UI work while Stop waits for the present thread.
   *
   * @param serve Waits up to the given time for UI work, runs it, and returns false once the UI
   *        has ended. An empty function serves nothing.
   */
  virtual void SetUIPump(MoveOnlyFunction<bool(std::chrono::milliseconds)> serve) = 0;
};

}  // namespace rex::gpu
