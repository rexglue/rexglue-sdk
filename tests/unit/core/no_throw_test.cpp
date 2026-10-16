/**
 * @file        tests/unit/core/no_throw_test.cpp
 * @brief       NoThrow turns an exception from a teardown step into a log line
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

#include <rex/api.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/logging/sink.h>
#include <rex/no_throw.h>

REX_TEST_CASE("NoThrow logs an exception from its step and returns", "[core][no_throw]") {
  rex::Shutdown();
  auto started = rex::Init({});
  REQUIRE(started);
  auto* logger = (*started)->Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);

  bool ran = false;
  rex::log::NoThrow(rex::log::core, [&] { ran = true; });
  rex::log::NoThrow(rex::log::core, [] { throw std::runtime_error("socket already closed"); });

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  rex::Shutdown();

  CHECK(ran);
  CHECK(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text == "teardown step failed, socket already closed";
  }));
}

REX_TEST_CASE("NoThrow is noexcept", "[core][no_throw]") {
  STATIC_REQUIRE(noexcept(rex::log::NoThrow(rex::log::core, [] {})));
}
