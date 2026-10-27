#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/first_seen.h"

TEST_CASE("FirstSeen reports a key once", "[first_seen]") {
  rex::FirstSeen seen;
  CHECK(seen.First("Icons\\"));
  CHECK_FALSE(seen.First("Icons\\"));
  CHECK_FALSE(seen.First("Icons\\"));
}

TEST_CASE("FirstSeen keeps distinct keys independent", "[first_seen]") {
  rex::FirstSeen seen;
  CHECK(seen.First("a"));
  CHECK(seen.First("b"));
  CHECK(seen.First(""));
  CHECK_FALSE(seen.First("a"));
  CHECK_FALSE(seen.First("b"));
  CHECK_FALSE(seen.First(""));
}

TEST_CASE("FirstSeen grants each key to exactly one thread", "[first_seen]") {
  rex::FirstSeen seen;
  constexpr int kThreads = 8;
  constexpr int kKeys = 64;
  std::atomic<int> firsts{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int k = 0; k < kKeys; ++k) {
        if (seen.First("key" + std::to_string(k))) {
          ++firsts;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(firsts.load() == kKeys);
}
