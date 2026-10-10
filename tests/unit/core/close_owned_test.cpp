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
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xobject.h>

#include "kernel/kernel_fixture.h"

using rex::runtime::AddOptions;
using rex::runtime::CloseOwned;
using rex::runtime::SymbolicLinkObject;
using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("CloseOwned closes the handle it is given", "[runtime][close_owned]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.runtime().object_table();
    auto added = table.Emplace<SymbolicLinkObject>(AddOptions{}, "a", "b");
    if (!added) {
      FAIL("the symbolic link was not added");
      return;
    }
    CloseOwned(table, *added);
    CHECK_FALSE(table.Close(*added));
  });
}

REX_TEST_CASE("CloseOwned logs a handle that does not close", "[runtime][close_owned]") {
  KernelFixture fixture;
  auto* logger = fixture.api().Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  fixture.RunOnGuestThread([](Caller& c) { CloseOwned(c.runtime().object_table(), 0xF8FFFFF0); });
  std::vector<rex::log::CapturedRecord> records;
  sink.CopySince(0, records);
  logger->RemoveSink(sink);
  CHECK(std::ranges::any_of(records, [](const rex::log::CapturedRecord& record) {
    return record.text.starts_with("Handle F8FFFFF0 did not close, ");
  }));
}
