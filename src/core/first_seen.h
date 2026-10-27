#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace rex {

// Tracks distinct keys for the life of the process so a repeated condition can
// be logged loudly once and quietly afterwards. Safe to call from any thread.
class FirstSeen {
 public:
  // Returns true only for the first call with a given key.
  bool First(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_.emplace(key).second;
  }

 private:
  std::mutex mutex_;
  std::unordered_set<std::string> seen_;
};

}  // namespace rex
