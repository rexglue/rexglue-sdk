/**
 * @file        tests/unit/base/thread_callback_test.cpp
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

#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>

#include "base/app_harness.h"

REX_TEST_CASE("A callback queued to an exited thread is dropped quietly", "[base][thread]") {
  rex::test::RunInInit(
      [] {
        rex::Logger& logger = REX_LOGGER();
        auto thread = rex::platform::Thread::Create({}, [] {});
        REQUIRE(thread);
        REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);

        auto ran = std::make_shared<std::atomic<bool>>(false);
        auto token = std::make_shared<int>(0);
        std::weak_ptr<int> alive = token;
        rex::log::CaptureSink sink;
        auto sink_guard = logger.AddSink(sink);
        thread->QueueUserCallback([ran, token = std::move(token)] { ran->store(true); });
        std::vector<rex::log::CapturedMessage> messages;
        sink.CopySince(0, messages);
        sink_guard.Reset();

        CHECK(alive.expired());
        CHECK_FALSE(ran->load());
        CHECK(std::ranges::none_of(messages, [](const rex::log::CapturedMessage& message) {
          return message.level >= rex::log::Level::kWarn;
        }));
        thread.reset();
      },
      {"--apu.driver=none", "--hid.drivers=none", "--ui.driver=none"});
}
