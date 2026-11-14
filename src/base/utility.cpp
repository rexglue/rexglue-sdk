#include <rex/utility.h>

#include <utility>

namespace rex {

ScopeGuard::ScopeGuard(MoveOnlyFunction<void()> release) : release_(std::move(release)) {}

ScopeGuard::ScopeGuard(ScopeGuard&& other) noexcept : release_(std::move(other.release_)) {
  other.release_ = nullptr;
}

ScopeGuard& ScopeGuard::operator=(ScopeGuard&& other) noexcept {
  if (this != &other) {
    Reset();
    release_ = std::move(other.release_);
    other.release_ = nullptr;
  }
  return *this;
}

ScopeGuard::~ScopeGuard() {
  Reset();
}

void ScopeGuard::Reset() {
  if (release_) {
    MoveOnlyFunction<void()> fn = std::move(release_);
    release_ = nullptr;
    fn();
  }
}

}  // namespace rex
