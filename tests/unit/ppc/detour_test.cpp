/**
 * @file        tests/unit/ppc/detour_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rex_test.h"

#include <rex/api.h>
#include <rex/logging/log.h>
#include <rex/logging/logger.h>
#include <rex/logging/sink.h>
#include <rex/image_info.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/context.h>
#include <rex/ppc/detour.h>
#include <rex/ppc/function.h>
#include <rex/ppc/stack.h>

#include "crash_child_runner.h"

namespace {

alignas(64) u8 g_mem[0x10000] = {};

constexpr u32 kStackTop = 0x8000;
constexpr u32 kEntryLR = 0x1234;

std::array<u32, 10> g_seen_by_body{};
std::array<u32, 10> g_seen_by_original{};

PPCContext MakeContext() {
  PPCContext ctx{};
  ctx.r1.u32 = kStackTop;
  ctx.r2.u64 = 0x2222;
  ctx.r13.u64 = 0;
  ctx.lr = kEntryLR;
  return ctx;
}

}  // namespace

extern "C" void __imp__DetourFnVoid(PPCContext& ctx, u8*) {
  ctx.r3.u64 = 7;
  ctx.r4.u64 = 8;
}

REX_DETOUR(DetourFnVoid, void, (u32 value)) {
  original(value);
}

extern "C" void __imp__DetourFnDouble(PPCContext& ctx, u8*) {
  ctx.r3.u64 = u64{ctx.r3.u32} * 2;
}

REX_DETOUR(DetourFnDouble, u32, (u32 value)) {
  return original(value) + 1;
}

extern "C" void __imp__DetourFnHalf(PPCContext& ctx, u8*) {
  ctx.f1.f64 = ctx.f1.f64 / 2;
}

REX_DETOUR(DetourFnHalf, f64, (f64 value)) {
  return original(value) + 0.25;
}

extern "C" void __imp__DetourFnTen(PPCContext& ctx, u8* base) {
  constexpr auto kPlaces = rex::ppc::PlaceArgs<u32, u32, u32, u32, u32, u32, u32, u32, u32, u32>();
  for (size_t i = 0; i < kPlaces.size(); ++i) {
    g_seen_by_original[i] = rex::ppc::ArgTranslator::GetValue<u32>(ctx, base, kPlaces[i]);
  }
}

REX_DETOUR(DetourFnTen, void,
           (PPCContext & ctx, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7,
            u32 a8, u32 a9)) {
  g_seen_by_body = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};
  rex::ppc::StackFrame frame(ctx, g_mem);
  const std::array<u8, 64> filler{};
  frame.PushBytes(filler);
  original(a0 + 100, a1 + 100, a2 + 100, a3 + 100, a4 + 100, a5 + 100, a6 + 100, a7 + 100, a8 + 100,
           a9 + 100);
}

extern "C" void __imp__DetourFnTenPlain(PPCContext& ctx, u8* base) {
  constexpr auto kPlaces = rex::ppc::PlaceArgs<u32, u32, u32, u32, u32, u32, u32, u32, u32, u32>();
  for (size_t i = 0; i < kPlaces.size(); ++i) {
    g_seen_by_original[i] = rex::ppc::ArgTranslator::GetValue<u32>(ctx, base, kPlaces[i]);
  }
}

REX_DETOUR(DetourFnTenPlain, void,
           (u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9)) {
  g_seen_by_body = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};
  original(a0 + 100, a1 + 100, a2 + 100, a3 + 100, a4 + 100, a5 + 100, a6 + 100, a7 + 100, a8 + 100,
           a9 + 100);
}

extern "C" void __imp__DetourFnInner(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r3.u32 + 100;
}

REX_DETOUR(DetourFnInner, u32, (u32 value)) {
  return original(value) + 1;
}

extern "C" void __imp__DetourFnOuter(PPCContext& ctx, u8* base) {
  DetourFnInner(ctx, base);
  ctx.r3.u64 += 10;
}

REX_DETOUR(DetourFnOuter, u32, (u32 value)) {
  return original(value) + 1000;
}

extern "C" void __imp__DetourFnPoke(PPCContext& ctx, u8*) {
  ctx.r3.u64 = 1;
}

REX_DETOUR(DetourFnPoke, void, (PPCContext & ctx)) {
  original();
  ctx.r11.u64 = 0x1234;
}

extern "C" void __imp__DetourFnStop(PPCContext&, u8*) {}

REX_DETOUR(DetourFnStop, void, (PPCContext & ctx)) {
  ctx.lr = 0xDEAD;
  ctx.r1.u32 -= 64;
  throw rex::ThreadStop{};
}

extern "C" void __imp__DetourFnThrow(PPCContext&, u8*) {}

REX_DETOUR(DetourFnThrow, void, (u32 value)) {
  if (value != 0) {
    throw std::runtime_error("detour body failed");
  }
  original(value);
}

REX_TEST_CASE("A void detour passes the original's results through", "[ppc][detour]") {
  PPCContext ctx = MakeContext();
  ctx.r3.u64 = 3;

  DetourFnVoid(ctx, g_mem);

  CHECK(ctx.r3.u32 == 7);
  CHECK(ctx.r4.u32 == 8);
}

REX_TEST_CASE("A detour leaves r1, r2, r13 and lr as it found them", "[ppc][detour]") {
  PPCContext ctx = MakeContext();

  DetourFnVoid(ctx, g_mem);

  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(ctx.r2.u64 == 0x2222);
  CHECK(ctx.r13.u64 == 0);
  CHECK(ctx.lr == kEntryLR);
  CHECK(rex::ppc::detail::GetInnermostDetourFrame() == nullptr);
}

REX_TEST_CASE("A typed detour returns its own value", "[ppc][detour]") {
  PPCContext ctx = MakeContext();

  const u32 result = rex::ppc::Call<u32>(ctx, g_mem, DetourFnDouble, u32{5});

  CHECK(result == 11);
}

REX_TEST_CASE("A float detour returns through f1", "[ppc][detour]") {
  PPCContext ctx = MakeContext();
  ctx.f1.f64 = 3.0;
  ctx.r3.u64 = 42;

  DetourFnHalf(ctx, g_mem);

  CHECK(ctx.f1.f64 == 1.75);
  CHECK(ctx.r3.u64 == 42);
}

REX_TEST_CASE("Stack arguments reach the detour body and the original after a push",
              "[ppc][detour]") {
  PPCContext ctx = MakeContext();
  g_seen_by_body = {};
  g_seen_by_original = {};

  rex::ppc::Call<void>(ctx, g_mem, DetourFnTen, u32{1}, u32{2}, u32{3}, u32{4}, u32{5}, u32{6},
                       u32{7}, u32{8}, u32{9}, u32{10});

  for (u32 i = 0; i < 10; ++i) {
    CHECK(g_seen_by_body[i] == i + 1);
    CHECK(g_seen_by_original[i] == i + 101);
  }
  CHECK(ctx.r1.u32 == kStackTop);
}

REX_TEST_CASE("Stack arguments reach the original in place when the body pushed nothing",
              "[ppc][detour]") {
  PPCContext ctx = MakeContext();
  g_seen_by_body = {};
  g_seen_by_original = {};

  rex::ppc::Call<void>(ctx, g_mem, DetourFnTenPlain, u32{1}, u32{2}, u32{3}, u32{4}, u32{5}, u32{6},
                       u32{7}, u32{8}, u32{9}, u32{10});

  for (u32 i = 0; i < 10; ++i) {
    CHECK(g_seen_by_body[i] == i + 1);
    CHECK(g_seen_by_original[i] == i + 101);
  }
  CHECK(ctx.r1.u32 == kStackTop);
}

REX_TEST_CASE("Nested detours restore their frames", "[ppc][detour]") {
  PPCContext ctx = MakeContext();

  const u32 result = rex::ppc::Call<u32>(ctx, g_mem, DetourFnOuter, u32{1});

  CHECK(result == 1112);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(ctx.lr == kEntryLR);
  CHECK(rex::ppc::detail::GetInnermostDetourFrame() == nullptr);
}

REX_TEST_CASE("A ctx detour can poke a non argument register", "[ppc][detour]") {
  PPCContext ctx = MakeContext();

  DetourFnPoke(ctx, g_mem);

  CHECK(ctx.r3.u32 == 1);
  CHECK(ctx.r11.u32 == 0x1234);
}

REX_TEST_CASE("A thread stop inside the detour body restores lr and propagates", "[ppc][detour]") {
  PPCContext ctx = MakeContext();

  CHECK_THROWS_AS(DetourFnStop(ctx, g_mem), rex::ThreadStop);

  CHECK(ctx.lr == kEntryLR);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(rex::ppc::detail::GetInnermostDetourFrame() == nullptr);
}

REX_TEST_CASE("A detour body that throws before running original does not assert",
              "[ppc][detour]") {
  PPCContext ctx = MakeContext();
  ctx.r3.u64 = 1;

  CHECK_THROWS_AS(DetourFnThrow(ctx, g_mem), std::runtime_error);

  CHECK(rex::ppc::detail::GetInnermostDetourFrame() == nullptr);
}

#ifndef NDEBUG
REX_TEST_CASE("A detour that returns without running original asserts in Debug",
              "[ppc][detour][crash_report]") {
  const auto result = rex::test::RunChild("detour-skips-original");
  CHECK(result.status != 0);
  CHECK(result.Has("returned without running its original"));
  CHECK_FALSE(result.Has("detour returned without its original"));
}

REX_TEST_CASE("A detour body that leaves a StackFrame open asserts in Debug",
              "[ppc][detour][crash_report]") {
  const auto result = rex::test::RunChild("detour-leaves-frame-open");
  CHECK(result.status != 0);
  CHECK(result.Has("left a StackFrame open"));
  CHECK_FALSE(result.Has("detour returned with a frame open"));
}
#endif

REX_TEST_CASE("A detour ctx write to a register kept in a local warns once", "[ppc][detour]") {
  rex::Shutdown();
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto* logger = api.value()->Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);

  const PPCContext at_entry = MakeContext();
  PPCContext at_exit = at_entry;
  at_exit.r20.u64 = 5;
  rex::PPCCodegenFlags flags{};
  flags.non_volatile_as_local = true;
  std::atomic_flag warned;

  const std::string first =
      rex::ppc::detail::CheckDetourLocalWrites("DetourFn", flags, at_entry, at_exit, warned);
  const std::string second =
      rex::ppc::detail::CheckDetourLocalWrites("DetourFn", flags, at_entry, at_exit, warned);

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  rex::Shutdown();
  CHECK(first == "r20");
  CHECK(second == "r20");
  CHECK(std::ranges::count_if(messages, [](const rex::log::CapturedMessage& message) {
          return message.text.contains("wrote r20 through ctx");
        }) == 1);
}

namespace {

struct LocalWriteCase {
  void (*mutate)(PPCContext&);
  bool rex::PPCCodegenFlags::* flag;
  const char* expected;
};

}  // namespace

REX_TEST_CASE("The detour register check follows the codegen flags", "[ppc][detour]") {
  const LocalWriteCase cases[] = {
      {[](PPCContext& ctx) { ctx.r20.u64 = 1; }, &rex::PPCCodegenFlags::non_volatile_as_local,
       "r20"},
      {[](PPCContext& ctx) { ctx.r31.u64 = 1; }, &rex::PPCCodegenFlags::non_volatile_as_local,
       "r31"},
      {[](PPCContext& ctx) { ctx.f14.u64 = 1; }, &rex::PPCCodegenFlags::non_volatile_as_local,
       "f14"},
      {[](PPCContext& ctx) { ctx.v14.u8[3] = 1; }, &rex::PPCCodegenFlags::non_volatile_as_local,
       "v14"},
      {[](PPCContext& ctx) { ctx.v127.u8[15] = 1; }, &rex::PPCCodegenFlags::non_volatile_as_local,
       "v127"},
      {[](PPCContext& ctx) { ctx.r0.u64 = 1; }, &rex::PPCCodegenFlags::non_argument_as_local, "r0"},
      {[](PPCContext& ctx) { ctx.r11.u64 = 1; }, &rex::PPCCodegenFlags::non_argument_as_local,
       "r11"},
      {[](PPCContext& ctx) { ctx.r12.u64 = 1; }, &rex::PPCCodegenFlags::non_argument_as_local,
       "r12"},
      {[](PPCContext& ctx) { ctx.f0.u64 = 1; }, &rex::PPCCodegenFlags::non_argument_as_local, "f0"},
      {[](PPCContext& ctx) { ctx.v32.u8[0] = 1; }, &rex::PPCCodegenFlags::non_argument_as_local,
       "v32"},
      {[](PPCContext& ctx) { ctx.v63.u8[0] = 1; }, &rex::PPCCodegenFlags::non_argument_as_local,
       "v63"},
      {[](PPCContext& ctx) { ctx.cr3.lt = 1; }, &rex::PPCCodegenFlags::cr_as_local, "cr3"},
      {[](PPCContext& ctx) { ctx.ctr.u64 = 9; }, &rex::PPCCodegenFlags::ctr_as_local, "ctr"},
      {[](PPCContext& ctx) { ctx.xer.ca = 1; }, &rex::PPCCodegenFlags::xer_as_local, "xer"},
      {[](PPCContext& ctx) { ctx.reserved.u64 = 9; }, &rex::PPCCodegenFlags::reserved_as_local,
       "reserved"},
  };
  const PPCContext at_entry = MakeContext();

  for (const LocalWriteCase& test : cases) {
    PPCContext at_exit = at_entry;
    test.mutate(at_exit);
    std::atomic_flag warned;
    warned.test_and_set();

    rex::PPCCodegenFlags other{};
    CHECK(rex::ppc::detail::CheckDetourLocalWrites("DetourFn", other, at_entry, at_exit, warned)
              .empty());

    rex::PPCCodegenFlags flags{};
    flags.*test.flag = true;
    CHECK(rex::ppc::detail::CheckDetourLocalWrites("DetourFn", flags, at_entry, at_exit, warned) ==
          test.expected);
  }
}

REX_TEST_CASE("The detour register check ignores lr and registers outside the flags",
              "[ppc][detour]") {
  const PPCContext at_entry = MakeContext();
  PPCContext at_exit = at_entry;
  at_exit.lr = 0x9999;
  at_exit.r4.u64 = 1;
  at_exit.r11.u64 = 1;
  at_exit.f1.u64 = 1;
  at_exit.v1.u8[0] = 1;
  std::atomic_flag warned;
  warned.test_and_set();
  rex::PPCCodegenFlags flags{};
  flags.non_volatile_as_local = true;
  flags.cr_as_local = true;
  flags.ctr_as_local = true;
  flags.xer_as_local = true;
  flags.reserved_as_local = true;
  flags.skip_lr = true;

  CHECK(rex::ppc::detail::CheckDetourLocalWrites("DetourFn", flags, at_entry, at_exit, warned)
            .empty());
}

REX_TEST_CASE("The detour covers the argument registers its parameters take", "[ppc][detour]") {
  constexpr auto kTwo = rex::ppc::detail::GetDeclaredArguments<void(u32, u32)>();
  STATIC_REQUIRE(kTwo.gpr_mask == 0x3);
  STATIC_REQUIRE(kTwo.fpr_mask == 0);

  constexpr auto kMixed = rex::ppc::detail::GetDeclaredArguments<void(u32, f64, u32)>();
  STATIC_REQUIRE(kMixed.gpr_mask == 0x7);
  STATIC_REQUIRE(kMixed.fpr_mask == 0x1);

  constexpr auto kFloat32First = rex::ppc::detail::GetDeclaredArguments<void(f32, u32)>();
  STATIC_REQUIRE(kFloat32First.gpr_mask == 0x3);
  STATIC_REQUIRE(kFloat32First.fpr_mask == 0x1);

  constexpr auto kFloat64First = rex::ppc::detail::GetDeclaredArguments<void(f64, u32)>();
  STATIC_REQUIRE(kFloat64First.gpr_mask == 0x3);
  STATIC_REQUIRE(kFloat64First.fpr_mask == 0x1);

  constexpr auto kNone = rex::ppc::detail::GetDeclaredArguments<void()>();
  STATIC_REQUIRE(kNone.gpr_mask == 0);
  STATIC_REQUIRE(kNone.fpr_mask == 0);
}

REX_TEST_CASE("A detour declaring fewer arguments than the function reads warns", "[ppc][detour]") {
  rex::Shutdown();
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto* logger = api.value()->Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);

  const rex::PPCCallUsage usage{0x82000100, 0x3, 0x1};
  const bool covered = rex::ppc::detail::CheckDetourArguments("DetourFn", usage, 0x3, 0x1);
  const bool short_gpr = rex::ppc::detail::CheckDetourArguments("DetourFn", usage, 0x1, 0x1);
  const bool short_fpr = rex::ppc::detail::CheckDetourArguments("DetourFn", usage, 0x3, 0);

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  rex::Shutdown();
  CHECK_FALSE(covered);
  CHECK(short_gpr);
  CHECK(short_fpr);
  const auto count = [&](std::string_view text) {
    return std::ranges::count_if(messages, [&](const rex::log::CapturedMessage& message) {
      return message.text.contains(text);
    });
  };
  CHECK(count("does not declare r4, which") == 1);
  CHECK(count("does not declare f1, which") == 1);
}

REX_TEST_CASE("A detour with no bound runtime skips the argument check", "[ppc][detour]") {
  rex::ppc::detail::CheckBoundDetourArguments("DetourFn", DetourFnVoid, 0, 0);
  SUCCEED();
}
