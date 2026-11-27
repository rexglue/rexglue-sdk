/**
 * @file        tests/unit/kernel/title_debug_output_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/title_debug_output.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/logger.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

REXLOG_CATEGORY(debugtext, "Debug text of the test title")
REXLOG_TABLE(kDebugTextTable, debugtext)

using rex::kernel::ITitleDebugOutput;
using rex::kernel::PrintfCall;
using rex::kernel::PrintfError;
using rex::kernel::TextEncoding;
using rex::log::Level;
using rex::ppc::ArgTranslator;
using rex::test::Caller;
using rex::test::KernelFixture;
using namespace std::string_view_literals;

namespace {

/** Captures the kernel messages at info level logged while it lives. */
class KernelLines {
 public:
  KernelLines() : guard_(REX_LOGGER().AddSink(sink_)), mark_(sink_.GetLatestSequence()) {}

  /** Returns the texts of the kernel info messages logged since construction. */
  std::vector<std::string> GetTexts() const {
    std::vector<rex::log::CapturedMessage> captured;
    sink_.CopySince(mark_, captured);
    std::vector<std::string> texts;
    for (const auto& message : captured) {
      if (message.category == "kernel" && message.level == Level::kInfo) {
        texts.push_back(message.text);
      }
    }
    return texts;
  }

 private:
  rex::log::LogCaptureSink sink_;
  rex::ScopeGuard guard_;
  u64 mark_;
};

void SetKernelLevel(std::string_view level) {
  REQUIRE(REX_SETTINGS().SetText("log.categories.kernel", level));
}

std::vector<std::string> RunCaptured(const std::function<void(Caller&)>& body) {
  KernelFixture fixture;
  SetKernelLevel("info");
  KernelLines lines;
  fixture.RunOnGuestThread(body);
  return lines.GetTexts();
}

ITitleDebugOutput& OutputOf(Caller& c) {
  return c.GetRuntime().GetDebugOutput();
}

void PutBytes(Caller& c, u32 address, std::string_view bytes) {
  u8* host = StateOf(c.GetRuntime()).GetAddressSpace()->TranslateVirtual<u8*>(address);
  std::memcpy(host, bytes.data(), bytes.size());
}

u32 AllocWideString(Caller& c, std::u16string_view text) {
  const u32 address = c.Alloc(static_cast<u32>((text.size() + 1) * 2));
  u8* host = StateOf(c.GetRuntime()).GetAddressSpace()->TranslateVirtual<u8*>(address);
  for (size_t i = 0; i < text.size(); ++i) {
    host[i * 2] = static_cast<u8>(text[i] >> 8);
    host[i * 2 + 1] = static_cast<u8>(text[i]);
  }
  return address;
}

/** Copies the register file of the thread, with the stack slots in a fresh guest block. */
std::unique_ptr<PPCContext> MakeCallContext(Caller& c) {
  auto ctx = std::make_unique<PPCContext>(c.GetContext());
  ctx->r1.u64 = c.Alloc(0x200);
  return ctx;
}

void SetArgument(Caller& c, PPCContext& ctx, size_t index, u64 value) {
  ArgTranslator::SetIntegerArgumentValue(ctx, c.GetBase(), index, value);
}

/** Reserves 2 blocks of 64 KiB and makes the second unreadable. Returns the first byte past it. */
u32 AllocReadableEdge(Caller& c) {
  const u32 base_ptr = c.Alloc(4);
  const u32 size_ptr = c.Alloc(4);
  const u32 old_ptr = c.Alloc(4);
  c.Store32(base_ptr, 0);
  c.Store32(size_ptr, 0x20000);
  REQUIRE(c.Kernel("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0x3000, 0x04, 0}) == 0);
  const u32 edge = c.Load32(base_ptr) + 0x10000;
  c.Store32(base_ptr, edge);
  c.Store32(size_ptr, 0x10000);
  REQUIRE(c.Kernel("NtProtectVirtualMemory", {base_ptr, size_ptr, 0x01, old_ptr, 0}) == 0);
  return edge;
}

PrintfError CodeOf(const rex::Result<std::string>& result) {
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == rex::ErrorCategory::kValidation);
  return static_cast<PrintfError>(result.error().code);
}

/** Runs body when step reaches turn, then moves step on. */
void TakeTurn(std::atomic<int>& step, int turn, const std::function<void()>& body) {
  for (int current = step.load(); current != turn; current = step.load()) {
    step.wait(current);
  }
  body();
  step.store(turn + 1);
  step.notify_all();
}

REX_FUNC(SetsResultToOne) {
  ctx.r3.u64 = 1;
}

REX_FUNC(PrintsAndRunsOriginal) {
  auto& out = rex::kernel::GetCurrent().GetDebugOutput();
  if (out.IsEnabled()) {
    if (auto text = PrintfCall::FromVarArgs(ctx, 1).Format()) {
      out.Write(*text, out.GetEncoding());
      out.Flush();
    }
  }
  SetsResultToOne(ctx, base);
}

}  // namespace

REX_TEST_CASE("Debug output joins fragments into one line", "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    ITitleDebugOutput& out = OutputOf(c);
    out.Write("[", TextEncoding::kUTF8);
    out.Write("a", TextEncoding::kUTF8);
    out.Write("]\n", TextEncoding::kUTF8);
  });
  CHECK(texts == std::vector<std::string>{"[a]"});
}

REX_TEST_CASE("Debug output removes carriage returns and skips empty lines",
              "[kernel][title_debug_output]") {
  const auto texts =
      RunCaptured([](Caller& c) { OutputOf(c).Write("a\r\nb\n\n\r\n", TextEncoding::kUTF8); });
  CHECK(texts == std::vector<std::string>{"a", "b"});
}

REX_TEST_CASE("Debug output logs a long line in parts that end on whole characters",
              "[kernel][title_debug_output]") {
  const std::string ascii(2500, 'x');
  std::string wide;
  for (int i = 0; i < 1000; ++i) {
    wide += "\xE3\x81\x82";
  }
  const auto texts = RunCaptured([&](Caller& c) {
    ITitleDebugOutput& out = OutputOf(c);
    out.Write(ascii, TextEncoding::kUTF8);
    out.Write("\n", TextEncoding::kUTF8);
    out.Write(wide, TextEncoding::kUTF8);
    out.Write("\n", TextEncoding::kUTF8);
  });
  REQUIRE(texts.size() == 6);
  CHECK(texts[0].size() == 1024);
  CHECK(texts[1].size() == 1024);
  CHECK(texts[2].size() == 452);
  CHECK(texts[0] + texts[1] + texts[2] == ascii);
  for (size_t i = 3; i < texts.size(); ++i) {
    CHECK(texts[i].size() <= 1024);
    CHECK(texts[i].size() % 3 == 0);
  }
  CHECK(texts[3] + texts[4] + texts[5] == wide);
}

REX_TEST_CASE("Debug output logs an unfinished line on Flush and at thread end",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  SetKernelLevel("info");
  KernelLines lines;
  fixture.RunOnGuestThread([&lines](Caller& c) {
    ITitleDebugOutput& out = OutputOf(c);
    out.Write("pending", TextEncoding::kUTF8);
    CHECK(lines.GetTexts().empty());
    out.Flush();
    CHECK(lines.GetTexts() == std::vector<std::string>{"pending"});
    out.Flush();
    CHECK(lines.GetTexts().size() == 1);
  });
  fixture.RunUntilGuestThreadEnds(
      [](Caller& c) { OutputOf(c).Write("unfinished", TextEncoding::kUTF8); });
  CHECK(lines.GetTexts() == std::vector<std::string>{"pending", "unfinished"});
}

REX_TEST_CASE("Debug output keeps the lines of two threads apart", "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    std::atomic<int> step{0};
    const u32 first = c.Spawn([&step](Caller& t) {
      ITitleDebugOutput& out = OutputOf(t);
      TakeTurn(step, 0, [&out] { out.Write("A1 ", TextEncoding::kUTF8); });
      TakeTurn(step, 2, [&out] { out.Write("A2\n", TextEncoding::kUTF8); });
    });
    const u32 second = c.Spawn([&step](Caller& t) {
      ITitleDebugOutput& out = OutputOf(t);
      TakeTurn(step, 1, [&out] { out.Write("B1 ", TextEncoding::kUTF8); });
      TakeTurn(step, 3, [&out] { out.Write("B2\n", TextEncoding::kUTF8); });
    });
    rex::test::Join(c, first);
    rex::test::Join(c, second);
    CHECK(step.load() == 4);
  });
  CHECK(texts == std::vector<std::string>{"A1 A2", "B1 B2"});
}

REX_TEST_CASE("Debug output logs nothing while the kernel category is below info",
              "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    ITitleDebugOutput& out = OutputOf(c);
    SetKernelLevel("warn");
    CHECK_FALSE(out.IsEnabled());
    out.Write("lost ", TextEncoding::kUTF8);
    out.Write("lost\n", TextEncoding::kUTF8);
    out.Write("lost", TextEncoding::kUTF8);
    SetKernelLevel("info");
    CHECK(out.IsEnabled());
    out.Write("kept\n", TextEncoding::kUTF8);
  });
  CHECK(texts == std::vector<std::string>{"kept"});
}

REX_TEST_CASE("Debug output converts Shift-JIS split across writes and replaces invalid UTF-8",
              "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    ITitleDebugOutput& out = OutputOf(c);
    out.Write("\x83", TextEncoding::kShiftJIS);
    out.Write("\x65\n", TextEncoding::kShiftJIS);
    out.Write("\x83", TextEncoding::kUTF8);
    out.Write("\x65\n", TextEncoding::kUTF8);
  });
  REQUIRE(texts.size() == 2);
  CHECK(texts[0] == "\xE3\x83\x86");
  CHECK(texts[1] ==
        "\xEF\xBF\xBD"
        "e");
}

REX_TEST_CASE("Debug output reads its encoding from kernel.debug_text_encoding",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  ITitleDebugOutput& out = fixture.GetRuntime().GetDebugOutput();
  CHECK(out.GetEncoding() == TextEncoding::kUTF8);
  REQUIRE(REX_SETTINGS().SetText("kernel.debug_text_encoding", "shift_jis"));
  CHECK(out.GetEncoding() == TextEncoding::kShiftJIS);
  REQUIRE(REX_SETTINGS().SetText("kernel.debug_text_encoding", "utf8"));
  CHECK(out.GetEncoding() == TextEncoding::kUTF8);
}

REX_TEST_CASE("PrintfCall formats a call with the format in r3", "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, c.AllocString("hello %d %s"));
    SetArgument(c, *ctx, 1, 42);
    SetArgument(c, *ctx, 2, c.AllocString("world"));
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "hello 42 world");
  });
}

REX_TEST_CASE("PrintfCall reads the arguments after a format in r4 from registers and stack slots",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, 0xFF00FF00);
    SetArgument(c, *ctx, 1, c.AllocString("%d %d %d %d %d %d %d %d"));
    for (u32 index = 2; index < 10; ++index) {
      SetArgument(c, *ctx, index, index - 1);
    }
    const auto text = PrintfCall::FromVarArgs(*ctx, 1).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "1 2 3 4 5 6 7 8");
  });
}

REX_TEST_CASE("PrintfCall reads the values of a va_list", "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    const u32 list = c.Alloc(16);
    c.Store64(list, 5);
    c.Store64(list + 8, c.AllocString("five"));
    SetArgument(c, *ctx, 0, c.AllocString("%d-%s"));
    SetArgument(c, *ctx, 1, list);
    const auto text = PrintfCall::FromVaList(*ctx, 0, 1).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "5-five");
  });
}

REX_TEST_CASE("PrintfCall formats a format held in the guest stack",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    const u32 format = c.GetContext().r1.u32 - 0x400;
    PutBytes(c, format, "stack %x\0"sv);
    SetArgument(c, *ctx, 0, format);
    SetArgument(c, *ctx, 1, 0xBEEF);
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "stack beef");
  });
}

REX_TEST_CASE("PrintfCall fails on an unreadable %s and prints a null one",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, c.AllocString("[%s]"));
    SetArgument(c, *ctx, 1, 0x10);
    CHECK(CodeOf(PrintfCall::FromVarArgs(*ctx, 0).Format()) == PrintfError::kUnreadableArgument);
    SetArgument(c, *ctx, 1, 0);
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "[(null)]");
  });
}

REX_TEST_CASE("PrintfCall fails on an unreadable or unterminated format",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    const auto code_at = [&](u32 format) {
      SetArgument(c, *ctx, 0, format);
      return CodeOf(PrintfCall::FromVarArgs(*ctx, 0).Format());
    };
    CHECK(code_at(0) == PrintfError::kUnreadableFormat);
    CHECK(code_at(0x10) == PrintfError::kUnreadableFormat);
    const u32 long_format = c.Alloc(2 * PrintfCall::kMaxFormatLength);
    PutBytes(c, long_format, std::string(PrintfCall::kMaxFormatLength, 'a'));
    CHECK(code_at(long_format) == PrintfError::kUnterminatedFormat);
    const u32 edge = AllocReadableEdge(c);
    PutBytes(c, edge - 16, std::string(16, 'b'));
    CHECK(code_at(edge - 16) == PrintfError::kUnterminatedFormat);
  });
}

REX_TEST_CASE("PrintfCall consumes %n and stores nothing", "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    const u32 word = c.Alloc(4);
    c.Store32(word, 0xDEADBEEF);
    SetArgument(c, *ctx, 0, c.AllocString("abc%d%n!"));
    SetArgument(c, *ctx, 1, 7);
    SetArgument(c, *ctx, 2, word);
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "abc7!");
    CHECK(c.Load32(word) == 0xDEADBEEF);
  });
}

REX_TEST_CASE("PrintfCall cuts the text at the bound", "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, c.AllocString("%s and more"));
    SetArgument(c, *ctx, 1, c.AllocString("0123456789abcdefghij"));
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format(16);
    REQUIRE(text.has_value());
    CHECK(*text == "0123456789abcdef");
  });
}

REX_TEST_CASE("PrintfCall leaves the register file unchanged", "[kernel][title_debug_output]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, c.AllocString("%s %d %d %d %d %d %d %d %d %d"));
    SetArgument(c, *ctx, 1, c.AllocString("text"));
    for (u32 index = 2; index < 11; ++index) {
      SetArgument(c, *ctx, index, index);
    }
    std::vector<u8> before(sizeof(PPCContext));
    std::memcpy(before.data(), ctx.get(), sizeof(PPCContext));
    const auto text = PrintfCall::FromVarArgs(*ctx, 0).Format();
    REQUIRE(text.has_value());
    CHECK(*text == "text 2 3 4 5 6 7 8 9 10");
    std::vector<u8> after(sizeof(PPCContext));
    std::memcpy(after.data(), ctx.get(), sizeof(PPCContext));
    CHECK(before == after);
  });
}

REX_TEST_CASE("A hook logs a printf call as one line and keeps the result of the original",
              "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    auto ctx = MakeCallContext(c);
    SetArgument(c, *ctx, 0, 0x00FF00FF);
    SetArgument(c, *ctx, 1, c.AllocString("hook %d"));
    SetArgument(c, *ctx, 2, 9);
    PrintsAndRunsOriginal(*ctx, c.GetBase());
    CHECK(ctx->r3.u64 == 1);
  });
  CHECK(texts == std::vector<std::string>{"hook 9"});
}

REX_TEST_CASE("OutputDebugStringA and OutputDebugStringW join into one UTF-8 line",
              "[kernel][title_debug_output]") {
  const auto texts = RunCaptured([](Caller& c) {
    c.XAM("OutputDebugStringA", {c.AllocString("one ")});
    c.XAM("OutputDebugStringA", {c.AllocString("two ")});
    c.XAM("OutputDebugStringW", {AllocWideString(c, u"caf\xE9\n")});
  });
  CHECK(texts == std::vector<std::string>{"one two caf\xC3\xA9"});
}

REX_TEST_CASE("Debug output logs every source in the category the title sets",
              "[kernel][title_debug_output]") {
  KernelFixture fixture;
  REQUIRE(REX_LOGGER().AddCategories(rex::log::kDebugTextTable));
  ITitleDebugOutput& out = fixture.GetRuntime().GetDebugOutput();
  CHECK(out.GetCategory().GetName() == "kernel");
  out.SetCategory(rex::log::debugtext);
  CHECK(&out.GetCategory() == &rex::log::debugtext);
  REQUIRE(REX_SETTINGS().SetText("log.categories.debugtext", "info"));
  SetKernelLevel("info");
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  const u64 mark = sink.GetLatestSequence();
  fixture.RunOnGuestThread([](Caller& c) {
    ITitleDebugOutput& output = OutputOf(c);
    output.Write("written ", TextEncoding::kUTF8);
    c.Kernel("DbgPrint", {c.AllocString("printed %d "), 3});
    PPCContext& ctx = c.GetContext();
    ctx.r3.u64 = c.AllocString("trapped\n");
    ctx.r4.u64 = 8;
    rex::ppc::Trap(ctx, c.GetBase(), 20);
    REQUIRE(REX_SETTINGS().SetText("log.categories.debugtext", "warn"));
    CHECK_FALSE(output.IsEnabled());
    output.Write("lost\n", TextEncoding::kUTF8);
  });
  sink_guard.Reset();
  REX_LOGGER().RemoveCategories(rex::log::kDebugTextTable);
  std::vector<rex::log::CapturedMessage> captured;
  sink.CopySince(mark, captured);
  std::vector<std::string> chosen;
  for (const auto& message : captured) {
    CHECK_FALSE((message.category == "kernel" && message.level == Level::kInfo));
    if (message.category == "debugtext") {
      CHECK(message.level == Level::kInfo);
      chosen.push_back(message.text);
    }
  }
  CHECK(chosen == std::vector<std::string>{"written printed 3 trapped"});
}
