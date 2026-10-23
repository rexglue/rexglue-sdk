/**
 * @file        tests/unit/core/process_resources_test.cpp
 * @brief       The open file limit rises to the hard limit and resource use is described
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <limits>
#include <string>

#include <rex/platform.h>
#include <rex/platform/process.h>

#if !REX_PLATFORM_WIN32
#include <sys/resource.h>
#endif

#if !REX_PLATFORM_WIN32
namespace {

class LimitGuard {
 public:
  LimitGuard() { ::getrlimit(RLIMIT_NOFILE, &saved_); }
  ~LimitGuard() { ::setrlimit(RLIMIT_NOFILE, &saved_); }
  LimitGuard(const LimitGuard&) = delete;
  LimitGuard& operator=(const LimitGuard&) = delete;
  const rlimit& Get() const { return saved_; }

 private:
  rlimit saved_{};
};

}  // namespace

REX_TEST_CASE("The open file limit rises from a low soft limit to the hard limit",
              "[core][process]") {
  LimitGuard guard;
  constexpr rlim_t kLow = 256;
  if (guard.Get().rlim_max != RLIM_INFINITY && guard.Get().rlim_max <= kLow) {
    SKIP("the hard limit is too low to lower the soft limit first");
  }
  rlimit low = guard.Get();
  low.rlim_cur = kLow;
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &low) == 0);

  const auto raised = rex::platform::RaiseOpenFileLimit();
  REQUIRE(raised);
  CHECK(raised->before == kLow);
  CHECK(raised->after > kLow);
  CHECK(raised->hard >= raised->after);

  rlimit now{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &now) == 0);
  CHECK(static_cast<rex::u64>(now.rlim_cur) == raised->after);
}

REX_TEST_CASE("Raising the open file limit again changes nothing", "[core][process]") {
  LimitGuard guard;
  const auto first = rex::platform::RaiseOpenFileLimit();
  REQUIRE(first);
  const auto second = rex::platform::RaiseOpenFileLimit();
  REQUIRE(second);
  CHECK(second->before == first->after);
  CHECK(second->after == second->before);
  CHECK(second->hard == first->hard);
}

REX_TEST_CASE("Resource use names the open descriptors", "[core][process]") {
  const std::string use = rex::platform::DescribeResourceUse();
  CHECK(use.starts_with("fds "));
  CHECK(use.find("maps ") != std::string::npos);
}
#else
REX_TEST_CASE("Windows has no open file limit to raise", "[core][process]") {
  const auto raised = rex::platform::RaiseOpenFileLimit();
  REQUIRE(raised);
  CHECK(raised->before == 0);
  CHECK(raised->after == 0);
  CHECK(raised->hard == 0);
}

REX_TEST_CASE("Resource use names the handle count", "[core][process]") {
  CHECK(rex::platform::DescribeResourceUse().starts_with("handles "));
}
#endif
