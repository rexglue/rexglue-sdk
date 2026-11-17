/**
 * @file        tests/unit/base/stub_log_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <string>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "base/app_harness.h"
#include "kernel/stub_log.h"
#include "xam/stub_log.h"

REX_STUB_RETURN(StubLogTestReturn, 5);
REX_STUB(StubLogTestPlain);

namespace {

using rex::test::RunInInit;

void KernelSite() {
  REX_KERNEL_STUB("stub log kernel site {}", 7);
}

void XamSite() {
  REX_XAM_STUB("stub log xam site");
}

size_t CountContaining(const rex::log::LogCaptureSink& sink, const std::string& text) {
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  size_t count = 0;
  for (const auto& line : lines) {
    count += line.text.find(text) != std::string::npos;
  }
  return count;
}

}  // namespace

REX_TEST_CASE("a stub warns on its first call and stays quiet at the default level",
              "[base][logging]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RunInInit([] {
    for (int i = 0; i < 5; ++i) {
      PPCContext ctx{};
      StubLogTestReturn(ctx, nullptr);
      CHECK(ctx.r3.u32 == 5);
      StubLogTestPlain(ctx, nullptr);
      KernelSite();
      XamSite();
    }
  });
  sink_guard.Reset();
  CHECK(CountContaining(sink, "StubLogTestReturn stub, returning 0x5") == 1);
  CHECK(CountContaining(sink, "StubLogTestPlain stub") == 1);
  CHECK(CountContaining(sink, "unimplemented: stub log kernel site 7") == 1);
  CHECK(CountContaining(sink, "unimplemented: stub log xam site") == 1);
}

REX_TEST_CASE("stub call cost", "[base][logging][benchmark]") {
  RunInInit([] {
    PPCContext ctx{};
    StubLogTestReturn(ctx, nullptr);
    KernelSite();
    BENCHMARK("REX_STUB_RETURN after the first call, logging at Info") {
      StubLogTestReturn(ctx, nullptr);
      return ctx.r3.u32;
    };
    BENCHMARK("REX_STUB after the first call, logging at Info") {
      StubLogTestPlain(ctx, nullptr);
      return ctx.r3.u32;
    };
    BENCHMARK("REX_KERNEL_STUB after the first call, logging at Info") {
      KernelSite();
    };
  });
}
