/**
 * @file        guest.h
 * @brief       Guest memory, write watch and interrupt interfaces
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {

struct PhysicalRange {
  u32 address = 0;
  u32 length = 0;
};

class IPhysicalWriteListener {
 public:
  virtual ~IPhysicalWriteListener() = default;

  /**
   * Called once per watched range that overlaps a write, on the writing thread under the write
   * watch lock. The watch fires once.
   *
   * @param range The written range, in whole pages.
   * @param exact True when only this exact range is unwatched.
   * @return The largest range around it that needs no watch, or {0, 0xFFFFFFFF} for none.
   */
  virtual PhysicalRange OnPhysicalWrite(PhysicalRange range, bool exact) = 0;
};

/** Guest physical memory. Each call fails for an unmapped range. */
class IPhysicalMemory {
 public:
  virtual ~IPhysicalMemory() = default;

  /**
   * Copies guest physical memory into a buffer.
   *
   * @param out The destination, sized to the length.
   * @return Ok, or an error when the range is not mapped.
   */
  [[nodiscard]] virtual Result<void> CopyFromPhysical(u32 address, std::span<std::byte> out) = 0;

  /**
   * Copies a buffer into guest physical memory and notifies the listeners of watched pages.
   *
   * @param data The bytes to copy.
   * @return Ok, or an error when the range is not mapped writable.
   */
  [[nodiscard]] virtual Result<void> CopyToPhysical(u32 address,
                                                    std::span<const std::byte> data) = 0;

  /**
   * Returns a read only view of guest physical memory, or an error when the range is not mapped.
   */
  [[nodiscard]] virtual Result<std::span<const std::byte>> GetReadOnlyPhysicalSpan(
      PhysicalRange range) = 0;

  /**
   * Returns a writable view of guest physical memory, or an error when not mapped writable. Writes
   * through it do not notify listeners.
   */
  [[nodiscard]] virtual Result<std::span<std::byte>> GetWritablePhysicalSpan(
      PhysicalRange range) = 0;

  /**
   * Writes a big endian u32 with release ordering, for GPU fences.
   *
   * @param address The 4 byte aligned address.
   * @return Ok, or an error when the address is not mapped writable.
   */
  [[nodiscard]] virtual Result<void> StoreRelease32(u32 address, u32 value) = 0;

  /**
   * Reads a big endian u32 with acquire ordering.
   *
   * @param address The 4 byte aligned address.
   * @return The value, or an error when the address is not mapped.
   */
  [[nodiscard]] virtual Result<u32> LoadAcquire32(u32 address) = 0;
};

class IPhysicalWriteWatch {
 public:
  virtual ~IPhysicalWriteWatch() = default;

  /**
   * Adds a write listener.
   *
   * @param listener The listener, which must outlive the guard.
   * @return The guard, destroyed to remove the listener.
   */
  [[nodiscard]] virtual ScopeGuard AddWriteListener(IPhysicalWriteListener& listener) = 0;

  /**
   * Watches a range, snapped to whole pages, until the next write to it.
   */
  virtual void WatchPhysicalWrites(PhysicalRange range) = 0;

  /**
   * Takes the write watch lock for a listener that updates its own watch state.
   *
   * @return The guard, destroyed to release the lock. Listeners run under it.
   */
  [[nodiscard]] virtual ScopeGuard LockWriteWatches() = 0;
};

/** Raises graphics interrupts into the title from host threads. */
class IGuestInterrupts {
 public:
  virtual ~IGuestInterrupts() = default;

  /**
   * Calls the title's interrupt callback on the calling thread, or does nothing until it sets one.
   *
   * @param source The interrupt source, such as vblank or a command processor event.
   * @param cpu The target guest CPU.
   * @return Ok, or an error when the calling thread did not come from StartHostThread.
   */
  [[nodiscard]] virtual Result<void> RaiseGraphicsInterrupt(u32 source, u32 cpu) = 0;

  /**
   * Starts a host thread that may call into the title, such as RaiseGraphicsInterrupt.
   *
   * @param fn The thread body, which returns when the stop token is requested.
   * @return The guard, destroyed to stop and join the thread.
   */
  [[nodiscard]] virtual Result<ScopeGuard> StartHostThread(
      std::string_view name, MoveOnlyFunction<void(std::stop_token)> fn) = 0;
};

}  // namespace rex
