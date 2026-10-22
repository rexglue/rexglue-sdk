/**
 * @file        tests/unit/runtime/call_usage_lookup_test.cpp
 * @brief       The runtime finds the argument registers an image records for a mapped function
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem/file_system.h>
#include <rex/image_info.h>
#include <rex/logging/logger.h>
#include <rex/logging/sink.h>
#include <rex/ppc/context.h>
#include <rex/ppc/detour.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>

#include "base_config.h"
#include "test_memory.h"

extern "C" void __imp__LookupNarrow(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r4.u64;
}

REX_DETOUR(LookupNarrow, u32, (u32 first)) {
  return original(first);
}

extern "C" void __imp__LookupCovered(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r4.u64;
}

REX_DETOUR(LookupCovered, u32, (u32 first, u32 second)) {
  return original(first, second);
}

namespace {

struct Fixture {
  Fixture() {
    rex::test::ReleaseTestMemory();
    rex::Shutdown();
  }
  ~Fixture() { rex::Shutdown(); }
};

void MappedUnlisted(PPCContext&, rex::u8*) {}

void MappedListed(PPCContext&, rex::u8*) {}

void NotMapped(PPCContext&, rex::u8*) {}

const PPCFuncMapping kMappings[] = {
    {0x82000100, MappedUnlisted},
    {0x82000200, MappedListed},
    {0x82000300, LookupNarrow},
    {0x82000400, LookupCovered},
    {0, nullptr},
};

const rex::PPCCallUsage kUsage[] = {
    {0x82000200, 0x5, 0x2},
    {0x82000300, 0x2, 0},
    {0x82000400, 0x2, 0},
    {0, 0, 0},
};

size_t CountMessages(const std::vector<rex::log::CapturedMessage>& messages,
                     std::string_view text) {
  return static_cast<size_t>(std::ranges::count_if(
      messages,
      [&](const rex::log::CapturedMessage& message) { return message.text.contains(text); }));
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture,
                     "call usage: the runtime looks a mapped function up by its host function",
                     "[runtime][call_usage]") {
  rex::Config config = rex::test::BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  rex::RuntimeDesc desc;
  desc.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  const rex::PPCImage image = {
      .code_base = 0x82000000,
      .code_size = 0x1000,
      .image_base = 0x82000000,
      .image_size = 0x10000,
      .func_mappings = kMappings,
      .call_usage = kUsage,
  };
  rex::Runtime runtime((*api)->GetConfig());
  REQUIRE(runtime.Setup(image, desc));

  const auto usage = runtime.GetCallUsage(MappedListed);
  REQUIRE(usage.has_value());
  const rex::PPCCallUsage found = usage.value_or(rex::PPCCallUsage{});
  CHECK(found.address == 0x82000200);
  CHECK(found.gpr_mask == 0x5);
  CHECK(found.fpr_mask == 0x2);
  CHECK_FALSE(runtime.GetCallUsage(MappedUnlisted).has_value());
  CHECK_FALSE(runtime.GetCallUsage(NotMapped).has_value());
  CHECK_FALSE(runtime.GetCallUsage(nullptr).has_value());
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "call usage: a hook is found by its symbol and checked on first call",
                     "[runtime][call_usage]") {
  rex::Config config = rex::test::BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto* logger = (*api)->Get<rex::ILogger>();
  REQUIRE(logger);
  rex::RuntimeDesc desc;
  desc.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  const rex::PPCImage image = {
      .code_base = 0x82000000,
      .code_size = 0x1000,
      .image_base = 0x82000000,
      .image_size = 0x10000,
      .func_mappings = kMappings,
      .call_usage = kUsage,
  };
  rex::Runtime runtime((*api)->GetConfig());
  REQUIRE(runtime.Setup(image, desc));

  const auto narrow = runtime.GetCallUsage(LookupNarrow);
  REQUIRE(narrow.has_value());
  CHECK(narrow.value_or(rex::PPCCallUsage{}).address == 0x82000300);

  alignas(64) static u8 memory[0x10000] = {};
  rex::runtime::ThreadState state(1, 0x8000, 0, runtime.GetMemory(), &runtime);
  rex::runtime::ThreadState* previous = rex::runtime::ThreadState::Get();
  rex::runtime::ThreadState::Bind(&state);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);

  PPCContext& ctx = *state.GetContext();
  ctx.r3.u64 = 1;
  ctx.r4.u64 = 2;
  LookupNarrow(ctx, memory);
  LookupNarrow(ctx, memory);
  LookupCovered(ctx, memory);
  LookupCovered(ctx, memory);

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  rex::runtime::ThreadState::Bind(previous);
  runtime.Shutdown();

#ifndef NDEBUG
  CHECK(CountMessages(messages, "LookupNarrow does not declare r4") == 1);
#else
  CHECK(CountMessages(messages, "LookupNarrow does not declare") == 0);
#endif
  CHECK(CountMessages(messages, "LookupCovered does not declare") == 0);
}
