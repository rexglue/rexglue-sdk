/**
 * @file        tests/unit/core/close_owned_test.cpp
 * @brief       CloseOwned closes a handle and logs a close that fails
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <vector>

#include <rex/api.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/logging/sink.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/object.h>

#include "kernel/kernel_fixture.h"

using rex::runtime::AddOptions;
using rex::runtime::CloseOwned;
using rex::runtime::SymbolicLinkObject;
using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("CloseOwned closes the handle it is given", "[core][close_owned]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.GetRuntime().GetObjectTable();
    auto added = table.Emplace<SymbolicLinkObject>(AddOptions{}, "a", "b");
    if (!added) {
      FAIL("the symbolic link was not added");
      return;
    }
    CloseOwned(table, *added);
    CHECK_FALSE(table.Close(*added));
  });
}

REX_TEST_CASE("CloseOwned logs a handle that does not close", "[core][close_owned]") {
  KernelFixture fixture;
  auto* logger = fixture.GetAPI().Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  fixture.RunOnGuestThread(
      [](Caller& c) { CloseOwned(c.GetRuntime().GetObjectTable(), 0xF8FFFFF0); });
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  CHECK(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text.starts_with("Handle F8FFFFF0 did not close, ");
  }));
}
