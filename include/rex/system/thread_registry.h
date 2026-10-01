/**
 * @file        system/thread_registry.h
 * @brief       Thread records, their registry and transient thread pins
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/system/pin_count.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>

namespace rex::system {

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
   * Tests whether the pin holds a record.
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

/** Owns every thread record and finds them by id or handle. */
class ThreadRegistry {
 public:
  /**
   * @param table The table that holds the thread handles.
   */
  explicit ThreadRegistry(util::ObjectTable* table) : table_(table) {}
  ~ThreadRegistry();

  /**
   * Returns the next thread id.
   *
   * @return A new id, never 0.
   */
  u32 NextID() { return ++next_id_; }

  /**
   * Takes ownership of a record, gives it a thread handle with a count of 1, allocates its title
   * thread block and starts its host thread.
   *
   * @param record The record.
   * @param out_handle Receives the creation handle.
   * @return X_STATUS_SUCCESS, or the status of the allocation or the thread creation.
   */
  X_STATUS Launch(std::unique_ptr<ThreadRecord> record, X_HANDLE* out_handle);

  /**
   * Finds a thread by id and pins it.
   *
   * @param thread_id The thread id.
   * @return The pin, empty when no record has the id.
   */
  ThreadPin Find(u32 thread_id);

  /**
   * Finds a thread by handle and pins it.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or names another kind.
   */
  ThreadPin FindByHandle(X_HANDLE handle);

  /**
   * Pins every record.
   *
   * @return The pins.
   */
  std::vector<ThreadPin> All();

  /**
   * Records that a thread finished running. The record is destroyed when its last handle is
   * already closed and no pin is held.
   *
   * @param record The record of the finished thread.
   */
  void NoteExited(ThreadRecord& record);

  /**
   * Records that the last handle of a thread closed. The record is destroyed when the thread
   * already finished and no pin is held.
   *
   * @param thread_id The thread id.
   */
  void NoteHandlesClosed(u32 thread_id);

 private:
  static void Dispose(std::unique_ptr<ThreadRecord> record);

  util::ObjectTable* table_;
  rex::platform::RankedSharedMutex mutex_{rex::platform::LockRank::ThreadRegistry};
  std::unordered_map<u32, std::unique_ptr<ThreadRecord>> threads_;
  std::atomic<u32> next_id_{0};
};

}  // namespace rex::system
