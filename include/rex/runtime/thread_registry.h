/**
 * @file        runtime/thread_registry.h
 * @brief       The thread registry interface, thread pins and fiber records
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

#include <rex/macros.h>
#include <rex/platform/fiber.h>
#include <rex/ppc/context.h>
#include <rex/result.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

/** A transient pin on a thread record. Never stored. Move only. */
class ThreadPin {
 public:
  ThreadPin() = default;
  /**
   * Adopts a pin already taken on a thread record.
   *
   * @param record The pinned record.
   */
  explicit ThreadPin(ThreadRecord* record) noexcept : record_(record) {}
  ThreadPin(ThreadPin&& other) noexcept : record_(other.record_) { other.record_ = nullptr; }
  ThreadPin& operator=(ThreadPin&& other) noexcept {
    if (this != &other) {
      Reset();
      record_ = other.record_;
      other.record_ = nullptr;
    }
    return *this;
  }
  ThreadPin(const ThreadPin&) = delete;
  ThreadPin& operator=(const ThreadPin&) = delete;
  ~ThreadPin() { Reset(); }

  /**
   * Reports whether the pin holds a record.
   *
   * @return True when it does.
   */
  explicit operator bool() const noexcept { return record_ != nullptr; }
  ThreadRecord* operator->() const noexcept { return record_; }
  ThreadRecord& operator*() const noexcept { return *record_; }

  /**
   * Returns the record.
   *
   * @return The record, null when empty.
   */
  [[nodiscard]] ThreadRecord* get() const noexcept { return record_; }

  /** Drops the pin. */
  void Reset() noexcept {
    if (record_ != nullptr) {
      if (record_->pins.Unpin()) {
        delete record_;
      }
      record_ = nullptr;
    }
  }

 private:
  ThreadRecord* record_ = nullptr;
};

/** Host data of a fiber the shim fiber routines manage. */
struct FiberInfo {
  rex::platform::Fiber* host_fiber; /**< The host fiber. */
  u32 uid;                          /**< A unique id for profiler names. */
  u32 guest_context_addr;           /**< The guest fiber context block. */
  u32 guest_stack_base;             /**< The top of the guest stack, 0 for a thread fiber. */
  u32 guest_stack_bottom;           /**< The bottom of the guest stack, 0 for a thread fiber. */
  bool is_thread_fiber; /**< True for ConvertThreadToFiber, whose stack the thread owns. */
};

/** Owns every thread record, finds them by id or handle, and keeps the TLS slots and fibers. */
class IThreadRegistry {
 public:
  REX_INTERFACE(IThreadRegistry, "rex.runtime.thread_registry", 1);
  virtual ~IThreadRegistry() = default;

  /**
   * Takes ownership of a record, assigns its thread id, gives it a thread handle with a count of
   * 1, allocates its guest thread block and starts its host thread.
   *
   * @param record The record.
   * @return The creation handle, or the status of the allocation or the thread creation.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Launch(std::unique_ptr<ThreadRecord> record) = 0;

  /**
   * Finds a thread by id and pins it.
   *
   * @param thread_id The thread id.
   * @return The pin, empty when no record has the id.
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
   * Pins every record.
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
   * Records a fiber, replacing a record with the same address.
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
   * Registers a thread notification. Every guest thread calls it at start and at exit.
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
