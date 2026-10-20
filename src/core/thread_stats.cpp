/**
 * @file        core/thread_stats.cpp
 * @brief       See rex/thread/thread_stats.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/thread/thread_stats.h>

#include <atomic>
#include <chrono>

#include <rex/thread.h>

namespace rex::thread_stats {

namespace {

// One per thread. Only its own thread writes it (relaxed adds); readers may
// see a wait's time a moment before its count, which a once-a-second reader
// doesn't notice.
struct Slot {
  std::atomic<uint32_t> system_id{0};
  std::atomic<uint64_t> wait_us[size_t(Wait::kCount)] = {};
  std::atomic<uint64_t> waits[size_t(Wait::kCount)] = {};
};

Slot g_slots[kMaxThreads];
std::atomic<size_t> g_slots_used{0};

// The calling thread's slot; nullptr until it first waits, and stays
// nullptr (with t_full set) if every slot was taken.
thread_local Slot* t_slot = nullptr;
thread_local bool t_full = false;

Slot* ThisThreadSlot() {
  if (t_slot || t_full) return t_slot;
  const size_t index = g_slots_used.fetch_add(1, std::memory_order_relaxed);
  if (index >= kMaxThreads) {
    t_full = true;
    return nullptr;
  }
  t_slot = &g_slots[index];
  // Published last: a reader skips slots whose id is still 0.
  t_slot->system_id.store(rex::thread::current_thread_system_id(), std::memory_order_release);
  return t_slot;
}

}  // namespace

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void AddWait(Wait kind, uint64_t micros) {
  Slot* slot = ThisThreadSlot();
  if (!slot) return;
  slot->wait_us[size_t(kind)].fetch_add(micros, std::memory_order_relaxed);
  slot->waits[size_t(kind)].fetch_add(1, std::memory_order_relaxed);
}

size_t Read(ThreadTotals* out, size_t max_threads) {
  size_t used = g_slots_used.load(std::memory_order_relaxed);
  if (used > kMaxThreads) used = kMaxThreads;
  size_t written = 0;
  for (size_t i = 0; i < used && written < max_threads; ++i) {
    const Slot& slot = g_slots[i];
    const uint32_t id = slot.system_id.load(std::memory_order_acquire);
    if (!id) continue;  // claimed, not yet published
    ThreadTotals& t = out[written++];
    t.system_id = id;
    for (size_t k = 0; k < size_t(Wait::kCount); ++k) {
      t.wait_us[k] = slot.wait_us[k].load(std::memory_order_relaxed);
      t.waits[k] = slot.waits[k].load(std::memory_order_relaxed);
    }
  }
  return written;
}

const char* Name(Wait kind) {
  switch (kind) {
    case Wait::kGuestObject: return "guest wait";
    case Wait::kGuestSleep: return "guest sleep";
    case Wait::kSpinLock: return "spinlock";
    case Wait::kGlobalLock: return "global lock";
    case Wait::kCount: break;
  }
  return "?";
}

}  // namespace rex::thread_stats
