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

std::vector<std::string> RunPrint(Level level, const std::function<void(Caller&)>& body) {
  KernelFixture fixture;
  REQUIRE(REX_SETTINGS().SetText("log.categories.kernel", rex::log::LevelName(level)));
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  const u64 mark = sink.GetLatestSequence();
  fixture.RunOnGuestThread(body);
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> captured;
  sink.CopySince(mark, captured);
  std::vector<std::string> texts;
  for (const auto& message : captured) {
    if (message.category == "kernel" && message.level == Level::kInfo) {
      texts.push_back(message.text);
    }
  }
  return texts;
}

}  // namespace

REX_TEST_CASE("DbgPrint and XamDbgPrint log the formatted text at Info in the kernel category",
              "[kernel][strings][dbgprint]") {
  const auto debug = RunPrint(Level::kInfo, [](Caller& c) {
    const u32 word = c.AllocString("world");
    CHECK(Print(c, BaseOf(c), __imp__DbgPrint, c.AllocString("hello %d %s\r\n"), {42, word}) == 0);
    CHECK(Print(c, BaseOf(c), __imp__XamDbgPrint, c.AllocString("xam %d\n"), {7}) == 0);
  });
  REQUIRE(debug.size() == 2);
  CHECK(debug[0] == "hello 42 world");
  CHECK(debug[1] == "xam 7");
}

REX_TEST_CASE("Two DbgPrint calls form one line", "[kernel][strings][dbgprint]") {
  const auto texts = RunPrint(Level::kInfo, [](Caller& c) {
    u8* const base = BaseOf(c);
    CHECK(Print(c, base, __imp__DbgPrint, c.AllocString("first %d, "), {1}) == 0);
    CHECK(Print(c, base, __imp__DbgPrint, c.AllocString("second %d\n"), {2}) == 0);
  });
  REQUIRE(texts.size() == 1);
  CHECK(texts[0] == "first 1, second 2");
}

REX_TEST_CASE(
    "DbgPrint and XamDbgPrint log nothing with the kernel category at Warn and still "
    "return 0",
    "[kernel][strings][dbgprint]") {
  const auto texts = RunPrint(Level::kWarn, [](Caller& c) {
    u8* const base = BaseOf(c);
    CHECK(Print(c, base, __imp__DbgPrint, c.AllocString("filtered %d\n"), {1}) == 0);
    CHECK(Print(c, base, __imp__XamDbgPrint, c.AllocString("filtered %d\n"), {1}) == 0);
    CHECK(Print(c, base, __imp__DbgPrint, 0) == 0);
  });
  CHECK(texts.empty());
}

REX_TEST_CASE("DbgPrint with the kernel category at Warn",
              "[kernel][strings][dbgprint][benchmark]") {
  KernelFixture fixture;
  REQUIRE(REX_SETTINGS().SetText("log.categories.kernel", rex::log::LevelName(Level::kWarn)));
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    const u32 format = c.AllocString("%s %d");
    const u32 word = c.AllocString("value");
    u8* const base = BaseOf(c);
    BENCHMARK("baseline: an empty function through the same Print") {
      return Print(c, base, NoOp, format, {word, 5});
    };
    BENCHMARK("DbgPrint(\"%s %d\") with the kernel category off") {
      return Print(c, base, __imp__DbgPrint, format, {word, 5});
    };
  });
}
