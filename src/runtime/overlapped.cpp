#include "runtime/overlapped.h"

namespace rex::runtime {

std::mutex& GetOverlappedMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace rex::runtime
