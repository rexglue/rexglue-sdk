/**
 * @file        tests/unit/kernel/dbgprint_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using namespace rex::ppc;
using rex::log::Level;
using rex::test::Caller;
using rex::test::KernelFixture;

extern "C" REX_FUNC(__imp__DbgPrint);
extern "C" REX_FUNC(__imp__XamDbgPrint);

namespace {

/** An export that does nothing, for the baseline of the call path. */
REX_FUNC(NoOp) {}

u8* BaseOf(Caller& c) {
  return StateOf(c.GetRuntime()).GetAddressSpace()->GetVirtualMembase();
}

u64 Print(Caller& c, u8* base, void (*print)(PPCContext&, u8*), u32 format,
          std::initializer_list<u64> arguments = {}) {
  PPCContext& ctx = c.GetContext();
  ArgTranslator::SetIntegerArgumentValue(ctx, base, 0, format);
  u32 index = 1;
  for (const u64 value : arguments) {
    ArgTranslator::SetIntegerArgumentValue(ctx, base, index++, value);
  }
  print(ctx, base);
  return ctx.r3.u64;
}

std::vector<std::string> RunPrint(Level level, std::string_view prefix,
                                  const std::function<void(Caller&)>& body) {
  KernelFixture fixture;
  REQUIRE(REX_SETTINGS().SetText("log.categories.title", rex::log::LevelName(level)));
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  fixture.RunOnGuestThread(body);
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> captured;
  sink.CopySince(0, captured);
  std::vector<std::string> texts;
  for (const auto& message : captured) {
    if (message.text.starts_with(prefix)) {
      texts.push_back(message.text);
    }
  }
  return texts;
}

}  // namespace

REX_TEST_CASE("DbgPrint and XamDbgPrint log the formatted text at Dev level",
              "[kernel][strings][dbgprint]") {
  const auto debug = RunPrint(Level::kDev, "DbgPrint: ", [](Caller& c) {
    const u32 word = c.AllocString("world");
    CHECK(Print(c, BaseOf(c), __imp__DbgPrint, c.AllocString("hello %d %s\r\n"), {42, word}) == 0);
  });
  REQUIRE(debug.size() == 1);
  CHECK(debug[0] == "DbgPrint: hello 42 world");
  const auto xam = RunPrint(Level::kDev, "XamDbgPrint: ", [](Caller& c) {
    CHECK(Print(c, BaseOf(c), __imp__XamDbgPrint, c.AllocString("xam %d"), {7}) == 0);
  });
  REQUIRE(xam.size() == 1);
  CHECK(xam[0] == "XamDbgPrint: xam 7");
}

REX_TEST_CASE("DbgPrint and XamDbgPrint log nothing above Dev level and still return 0",
              "[kernel][strings][dbgprint]") {
  const auto texts = RunPrint(Level::kInfo, "", [](Caller& c) {
    u8* const base = BaseOf(c);
    CHECK(Print(c, base, __imp__DbgPrint, c.AllocString("filtered %d"), {1}) == 0);
    CHECK(Print(c, base, __imp__XamDbgPrint, c.AllocString("filtered %d"), {1}) == 0);
    CHECK(Print(c, base, __imp__DbgPrint, 0) == 0);
  });
  for (const std::string& text : texts) {
    CHECK_FALSE(text.starts_with("DbgPrint: "));
    CHECK_FALSE(text.starts_with("XamDbgPrint: "));
  }
}

REX_TEST_CASE("DbgPrint with the level at Info", "[kernel][strings][dbgprint][benchmark]") {
  KernelFixture fixture;
  REQUIRE(REX_SETTINGS().SetText("log.categories.title", rex::log::LevelName(Level::kInfo)));
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    const u32 format = c.AllocString("%s %d");
    const u32 word = c.AllocString("value");
    u8* const base = BaseOf(c);
    BENCHMARK("baseline: an empty function through the same Print") {
      return Print(c, base, NoOp, format, {word, 5});
    };
    BENCHMARK("DbgPrint(\"%s %d\") with Dev off") {
      return Print(c, base, __imp__DbgPrint, format, {word, 5});
    };
  });
}
