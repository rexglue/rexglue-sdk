/**
 * @file        runtime/thread_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/fiber.h>
#include <rex/ppc/context.h>
#include <rex/result.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::runtime {

/** A transient pin on a `KernelThread`. Never stored. Move only. */
class ThreadPin : public rex::MoveOnly {
 public:
  ThreadPin() = default;
  /**
   * Takes a pin already taken on a `KernelThread`.
   *
   * @param thread The pinned `KernelThread`.
   */
  explicit ThreadPin(KernelThread* thread) noexcept : thread_(thread) {}
  ThreadPin(ThreadPin&& other) noexcept : thread_(other.thread_) { other.thread_ = nullptr; }
  /**
   * Takes over the pin of another pin object and releases the pin this pin held.
   *
   * @param other The pin left empty.
   * @return This pin.
   */
  ThreadPin& operator=(ThreadPin&& other) noexcept {
    if (this != &other) {
      Reset();
      thread_ = other.thread_;
      other.thread_ = nullptr;
    }
    return *this;
  }
  ~ThreadPin() { Reset(); }

  /**
   * Reports whether the pin holds a `KernelThread`.
   *
   * @return True when it does.
   */
  explicit operator bool() const noexcept { return thread_ != nullptr; }
  KernelThread* operator->() const noexcept { return thread_; }
  KernelThread& operator*() const noexcept { return *thread_; }

  /**
   * Returns the `KernelThread`.
   *
   * @return The `KernelThread`, null when empty.
   */
  [[nodiscard]] KernelThread* GetThread() const noexcept { return thread_; }

  /** Drops the pin. */
  void Reset() noexcept {
    if (thread_ != nullptr) {
      if (thread_->pins.Unpin()) {
        delete thread_;
      }
      thread_ = nullptr;
    }
  }

 private:
  KernelThread* thread_ = nullptr;
};

/** Host data of a fiber the shim fiber routines manage. */
struct FiberInfo {
  rex::platform::Fiber* host_fiber; /**< The host fiber. */
  u32 uid;                          /**< A unique id for profiler names. */
  u32 guest_context_addr;           /**< The guest fiber context block. */
  u32 guest_stack_bottom; /**< The first byte of the guest stack allocation, 0 for a thread fiber.
                           */
  bool is_thread_fiber;   /**< True for ConvertThreadToFiber, whose stack the thread owns. */
};

/** Owns every `KernelThread`, finds them by id or handle, and keeps the TLS slots and fibers. */
REX_INTERFACE(IThreadRegistry, "rex.runtime.thread_registry", 1)
 public:
  /**
   * Takes ownership of a `KernelThread`, assigns its thread id, gives it a thread handle with a
   * count of 1, allocates its guest thread block and starts its host thread.
   *
   * @param thread The `KernelThread`.
   * @return The creation handle, or the status of the allocation or the thread creation.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Launch(std::unique_ptr<KernelThread> thread) = 0;

  /**
   * Finds a thread by id and pins it.
   *
   * @param thread_id The thread id.
   * @return The pin, empty when no `KernelThread` has the id.
   */
  [[nodiscard]] virtual ThreadPin Find(u32 thread_id) = 0;

  /**
   * Finds a thread by handle and pins it.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or names another kind.
   */
  [[nodiscard]] virtual ThreadPin FindByHandle(X_HANDLE handle) = 0;

  /**
   * Pins every `KernelThread`.
   *
   * @return The pins.
   */
  [[nodiscard]] virtual std::vector<ThreadPin> All() = 0;

  /**
   * Claims a TLS slot in the process of the calling guest thread under the guest TLS spinlock.
   *
   * @param context The PPC context of the calling thread.
   * @return The slot, empty when the process has no free slot or the caller is no guest thread.
   */
  [[nodiscard]] virtual std::optional<u32> AllocateTLS(PPCContext* context) = 0;

  /**
   * Releases a TLS slot and clears it in every thread of the calling thread's process.
   *
   * @param context The PPC context of the calling thread.
   * @param slot The slot.
   */
  virtual void FreeTLS(PPCContext* context, u32 slot) = 0;

  /**
   * Finds a fiber by its guest context block.
   *
   * @param address The guest address of the fiber context block.
   * @return A copy of the fiber data, empty when no fiber has the address.
   */
  [[nodiscard]] virtual std::optional<FiberInfo> LookupFiber(u32 address) = 0;

  /**
   * Registers a fiber, replacing the fiber with the same address.
   *
   * @param address The guest address of the fiber context block.
   * @param info The fiber data.
   */
  virtual void RegisterFiber(u32 address, const FiberInfo& info) = 0;

  /**
   * Forgets a fiber.
   *
   * @param address The guest address of the fiber context block.
   */
  virtual void UnregisterFiber(u32 address) = 0;

  /**
   * Returns a fiber name for the profiler. Names are never freed because the profiler keeps the
   * pointers.
   *
   * @param address The fiber uid the name is keyed by.
   * @param thread_name The name of the thread that owns the fiber.
   * @return The name.
   */
  [[nodiscard]] virtual const char* GetOrCreateFiberName(u32 address, const char* thread_name) = 0;

  /**
   * Registers a thread notification.
   *
   * @param notification Address of the title's XTHREAD_NOTIFICATION.
   */
  virtual void RegisterThreadNotify(u32 notification) = 0;

  /**
   * Removes a thread notification.
   *
   * @param notification Address of the title's XTHREAD_NOTIFICATION.
   */
  virtual void RemoveThreadNotify(u32 notification) = 0;
};

}  // namespace rex::runtime
