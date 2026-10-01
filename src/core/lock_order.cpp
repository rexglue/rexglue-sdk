#include <rex/platform/lock_order.h>

#include <array>
#include <atomic>

#include <rex/assert.h>

namespace rex::platform {

namespace {

void DefaultHandler(LockRank, LockRank) {
  assert_always("lock order violation");
}

std::atomic<LockOrderHandler> g_handler{&DefaultHandler};

#ifndef NDEBUG
constexpr size_t kMaxHeld = 16;

struct HeldStack {
  std::array<LockRank, kMaxHeld> ranks;
  size_t depth = 0;
};

thread_local HeldStack t_held;
#endif

}  // namespace

LockOrderHandler SetLockOrderHandler(LockOrderHandler handler) {
  return g_handler.exchange(handler ? handler : &DefaultHandler);
}

void NoteLockAcquire(LockRank rank) {
#ifndef NDEBUG
  HeldStack& held = t_held;
  if (held.depth > 0) {
    const LockRank top = held.ranks[held.depth - 1];
    const bool violation = top == LockRank::Leaf || (rank != LockRank::Leaf &&
                                                     static_cast<u8>(rank) <= static_cast<u8>(top));
    if (violation) {
      g_handler.load()(top, rank);
    }
  }
  if (held.depth < kMaxHeld) {
    held.ranks[held.depth++] = rank;
  }
#else
  (void)rank;
#endif
}

void NoteLockRelease(LockRank rank) {
#ifndef NDEBUG
  HeldStack& held = t_held;
  for (size_t i = held.depth; i > 0; --i) {
    if (held.ranks[i - 1] == rank) {
      for (size_t j = i; j < held.depth; ++j) {
        held.ranks[j - 1] = held.ranks[j];
      }
      --held.depth;
      return;
    }
  }
#else
  (void)rank;
#endif
}

}  // namespace rex::platform
