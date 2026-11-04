/**
 * @file        tests/unit/base/no_throw_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/logging/log.h>

#include "base/app_harness.h"

REX_TEST_CASE("NoThrow logs an exception from its step and returns", "[base][no_throw]") {
  bool ran = false;
  std::vector<rex::log::CapturedMessage> messages;
  rex::test::RunInInit([&] {
    rex::Logger& logger = REX_LOGGER();
    rex::log::LogCaptureSink sink;
    auto sink_guard = logger.AddSink(sink);

    rex::log::NoThrow(rex::log::core, [&] { ran = true; });
    rex::log::NoThrow(rex::log::core, [] { throw std::runtime_error("socket already closed"); });

    sink.CopySince(0, messages);
    sink_guard.Reset();
  });

  CHECK(ran);
  CHECK(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text == "teardown step failed, socket already closed";
  }));
}

REX_TEST_CASE("NoThrow is noexcept", "[base][no_throw]") {
  STATIC_REQUIRE(noexcept(rex::log::NoThrow(rex::log::core, [] {})));
}
