/**
 * @file        gpu/xenos_graphics_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/gpu/graphics_driver.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * A Xenos GPU. It adds what the guest kernel needs beyond presenting. GraphicsSystem routes the
 * kernel's calls here only when the title requires the Xenos family.
 */
class IXenosGraphicsDriver : public IGraphicsDriver {
 public:
  IXenosGraphicsDriver* AsXenos() final { return this; }

  /**
   * Starts reading commands from the guest ring buffer. Guest thread.
   *
   * @param size_log2 Log2 of its size in quadwords.
   */
  virtual void InitializeRingBuffer(u32 address, u32 size_log2) = 0;

  /**
   * Writes the ring's read pointer to guest memory as commands complete. Guest thread.
   *
   * @param address Guest physical address to write it to.
   * @param block_size_log2 Log2 of the quadwords between writes.
   */
  virtual void EnableReadPointerWriteBack(u32 address, u32 block_size_log2) = 0;

  /** Reads a GPU register for the guest. Any guest thread. */
  virtual u32 ReadRegister(u32 index) = 0;

  /** Writes a GPU register for the guest. Any guest thread. */
  virtual void WriteRegister(u32 index, u32 value) = 0;
};

}  // namespace rex::gpu
