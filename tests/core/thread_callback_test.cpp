/**
 * @file        tests/core/thread_callback_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include <rex/api.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/platform/thread.h>

REX_TEST_CASE("A callback queued to an exited thread is dropped quietly", "[core][thread]") {
  rex::Shutdown();
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto* logger = api.value()->Get<rex::ILogger>();
  REQUIRE(logger);
  auto thread = rex::platform::Thread::Create({}, [] {});
  REQUIRE(thread);
  REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);

  auto ran = std::make_shared<std::atomic<bool>>(false);
  auto token = std::make_shared<int>(0);
  std::weak_ptr<int> alive = token;
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  thread->QueueUserCallback([ran, token = std::move(token)] { ran->store(true); });
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);

  CHECK(alive.expired());
  CHECK_FALSE(ran->load());
  CHECK(std::ranges::none_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.level >= rex::log::Level::kWarn;
  }));
  thread.reset();
  rex::Shutdown();
}
