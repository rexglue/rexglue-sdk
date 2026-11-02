/**
 * @file        tests/unit/kernel/shim_debug_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/call.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using namespace rex::ppc;
using rex::log::Level;
using rex::test::Caller;
using rex::test::KernelFixture;

extern "C" REX_FUNC(rexshim_DebugPrint);

namespace {

void Print(Caller& c, u32 format, std::initializer_list<u64> arguments = {}) {
  PPCContext& ctx = c.GetContext();
  u8* base = StateOf(c.GetRuntime()).GetMemory()->GetVirtualMembase();
  ArgTranslator::SetIntegerArgumentValue(ctx, base, 0, format);
  u32 index = 1;
  for (const u64 value : arguments) {
    ArgTranslator::SetIntegerArgumentValue(ctx, base, index++, value);
  }
  rexshim_DebugPrint(ctx, base);
}

std::vector<std::string> RunPrint(const std::optional<Level>& level,
                                  const std::function<void(Caller&)>& body) {
  KernelFixture fixture;
  rex::Logger* logger = &REX_LOGGER();
  REQUIRE(logger);
  REQUIRE(REX_SETTINGS().SetText("log.categories.kernel",
                                 level ? rex::log::LevelName(*level) : std::string_view{}));
  rex::log::CaptureSink sink;
  auto sink_guard = logger->AddSink(sink);
  fixture.RunOnGuestThread(body);
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> captured;
  sink.CopySince(0, captured);
  std::vector<std::string> texts;
  for (const auto& message : captured) {
    if (message.text.starts_with("DebugPrint: ")) {
      texts.push_back(message.text);
    }
  }
  return texts;
}

}  // namespace

REX_TEST_CASE("DebugPrint formats the message and logs it at Debug level",
              "[kernel][shim][debug_print]") {
  const auto texts = RunPrint(Level::kDebug, [](Caller& c) {
    const u32 format = c.AllocString("hello %d %s\r\n");
    const u32 word = c.AllocString("world");
    Print(c, format, {42, word});
  });
  REQUIRE(texts.size() == 1);
  CHECK(texts[0] == "DebugPrint: hello 42 world");
}

REX_TEST_CASE("DebugPrint converts Shift-JIS text to UTF-8", "[kernel][shim][debug_print]") {
  const auto texts = RunPrint(Level::kDebug, [](Caller& c) {
    const u32 format = c.AllocString(
        "\x83\x65\x83\x58\x83\x67"
        " %d");
    Print(c, format, {7});
  });
  REQUIRE(texts.size() == 1);
  CHECK(texts[0] ==
        "DebugPrint: "
        "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"
        " 7");
}

REX_TEST_CASE("DebugPrint logs nothing below Debug level", "[kernel][shim][debug_print]") {
  const auto texts = RunPrint(Level::kInfo, [](Caller& c) {
    const u32 format = c.AllocString("filtered %d");
    Print(c, format, {1});
  });
  CHECK(texts.empty());
}

REX_TEST_CASE("DebugPrint ignores a format pointer that is not readable text",
              "[kernel][shim][debug_print]") {
  const auto texts = RunPrint(Level::kDebug, [](Caller& c) {
    Print(c, 0);
    Print(c, 0x10);
    Print(c, 0xFFFFFFF0);
    const u32 blank = c.AllocString(" \r\n");
    Print(c, blank);
  });
  CHECK(texts.empty());
}
