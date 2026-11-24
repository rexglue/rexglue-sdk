/**
 ******************************************************************************
 * The few threading primitives this backend used from rex/thread.h.
 ******************************************************************************
 *
 * Upstream no longer ships rex/thread.h. Fourteen lines in the ported backend use it, and they are these: a thread that
 * is created, joined and waited on; a processor count; a sleep; a yield; and a global critical region. Everything here is
 * a straight bridge to the standard library, so the call sites keep reading naturally while the behaviour is std's.
 *
 * This file exists so the PR is small. If a reviewer prefers `std::thread` spelled out at the call sites, deleting this
 * header and fixing the fourteen lines is a mechanical pass with a compiler to guide it.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::thread {

class Thread {
 public:
  // Thread names are a debugger convenience the SDK exposed; std::thread has no such call, so this is a no-op.
  void SetName(const char*) {}
  // Kept for call-site compatibility; the standard library has nothing to configure here.
  struct CreationParameters {};

  template <typename F>
  static std::unique_ptr<Thread> Create(const CreationParameters&, F&& fn) {
    auto thread = std::unique_ptr<Thread>(new Thread());
    thread->thread_ = std::thread(std::forward<F>(fn));
    return thread;
  }

  bool joinable() const { return thread_.joinable(); }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  ~Thread() {
    // A joinable std::thread would terminate the process; the old rex thread was detached on destruction instead.
    if (thread_.joinable()) {
      thread_.detach();
    }
  }

 private:
  Thread() = default;
  std::thread thread_;

};

inline void Wait(Thread* thread, bool /*infinite*/) {
  if (thread && thread->joinable()) {
    thread->Join();
  }
}

inline uint32_t logical_processor_count() {
  return std::max(1u, std::thread::hardware_concurrency());
}

inline void sleep(std::chrono::microseconds duration) { std::this_thread::sleep_for(duration); }

inline void yield() { std::this_thread::yield(); }

// Upstream used one global critical region to serialise guest-memory watchers; the port only needs the lock itself.
class global_critical_region {
 public:
  static std::recursive_mutex& Mutex() {
    static std::recursive_mutex mutex;
    return mutex;
  }
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(Mutex());
  }
};

}  // namespace rex::thread
