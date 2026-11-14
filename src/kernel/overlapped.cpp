#include "kernel/overlapped.h"

namespace rex::kernel {

std::mutex& GetOverlappedMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace rex::kernel
