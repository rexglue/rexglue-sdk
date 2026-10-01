#include <rex/platform/interrupt_mask.h>

#include <atomic>

#include <rex/assert.h>

namespace rex::platform {

namespace {

std::recursive_mutex& Mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

std::atomic<int>& Depth() {
  static std::atomic<int> depth{0};
  return depth;
}

}  // namespace

void InterruptMask::Disable() {
  Mutex().lock();
  Depth().fetch_add(1);
}

void InterruptMask::Enable() {
  const int previous = Depth().fetch_sub(1);
  assert_true(previous >= 1);
  Mutex().unlock();
}

bool InterruptMask::Enabled() {
  std::lock_guard lock(Mutex());
  return Depth().load() == 0;
}

InterruptMask::Guard::Guard() {
  Mutex().lock();
}

InterruptMask::Guard::~Guard() {
  Mutex().unlock();
}

}  // namespace rex::platform
