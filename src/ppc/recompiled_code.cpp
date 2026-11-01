#include <rex/ppc/recompiled_code.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace rex::ppc {

namespace {

struct Slot {
  std::atomic<u64> begin{0};
  std::atomic<u64> end{0};
  u32 count = 0;
};

std::array<Slot, 64> g_slots;
std::mutex g_slots_mutex;

u64 Begin(std::span<const u8> code) {
  return reinterpret_cast<u64>(code.data());
}

}  // namespace

bool AddRecompiledCode(std::span<const u8> code) {
  if (code.empty()) {
    return true;
  }
  const u64 begin = Begin(code);
  const u64 end = begin + code.size();
  std::lock_guard lock(g_slots_mutex);
  for (Slot& slot : g_slots) {
    if (slot.count > 0 && slot.begin.load(std::memory_order_relaxed) == begin &&
        slot.end.load(std::memory_order_relaxed) == end) {
      ++slot.count;
      return true;
    }
  }
  for (Slot& slot : g_slots) {
    if (slot.count == 0) {
      slot.count = 1;
      slot.end.store(end, std::memory_order_relaxed);
      slot.begin.store(begin, std::memory_order_release);
      return true;
    }
  }
  return false;
}

void RemoveRecompiledCode(std::span<const u8> code) {
  const u64 begin = Begin(code);
  const u64 end = begin + code.size();
  std::lock_guard lock(g_slots_mutex);
  for (Slot& slot : g_slots) {
    if (slot.count > 0 && slot.begin.load(std::memory_order_relaxed) == begin &&
        slot.end.load(std::memory_order_relaxed) == end) {
      if (--slot.count == 0) {
        slot.begin.store(0, std::memory_order_release);
        slot.end.store(0, std::memory_order_relaxed);
      }
      return;
    }
  }
}

bool IsRecompiledCode(u64 address) {
  return std::ranges::any_of(g_slots, [address](const Slot& slot) {
    const u64 begin = slot.begin.load(std::memory_order_acquire);
    return begin != 0 && address >= begin && address < slot.end.load(std::memory_order_relaxed);
  });
}

}  // namespace rex::ppc
