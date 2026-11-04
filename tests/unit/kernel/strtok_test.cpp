/**
 * @file        tests/unit/kernel/strtok_test.cpp
 * @brief       The shim strtok keeps its scan position per guest thread
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <rex/ppc/context.h>

#include "kernel_fixture.h"

extern "C" void rexshim_strtok(PPCContext& ctx, rex::u8* base);

using rex::u32;
using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

std::string Tokenize(Caller& c, u32 text, u32 delim) {
  c.GetContext().r3.u64 = text;
  c.GetContext().r4.u64 = delim;
  rexshim_strtok(c.GetContext(), c.GetBase());
  const u32 token = c.GetContext().r3.u32;
  return token ? std::string(reinterpret_cast<const char*>(c.GetBase() + token)) : std::string();
}

bool WaitForStep(const std::atomic<int>& step, int value) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (step.load() < value) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

REX_TEST_CASE("Two guest threads tokenize different strings without sharing a position",
              "[kernel][strtok]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    struct Result {
      std::string first;
      std::string second;
    };
    auto a = std::make_shared<Result>();
    auto b = std::make_shared<Result>();
    auto step = std::make_shared<std::atomic<int>>(0);
    const u32 delim = c.AllocString(",");
    const u32 text_a = c.AllocString("a1,a2");
    const u32 text_b = c.AllocString("b1,b2");

    const u32 thread_a = c.Spawn([=](Caller& t) {
      a->first = Tokenize(t, text_a, delim);
      step->store(1);
      if (WaitForStep(*step, 2)) {
        a->second = Tokenize(t, 0, delim);
      }
      step->store(3);
    });
    const u32 thread_b = c.Spawn([=](Caller& t) {
      if (WaitForStep(*step, 1)) {
        b->first = Tokenize(t, text_b, delim);
      }
      step->store(2);
      if (WaitForStep(*step, 3)) {
        b->second = Tokenize(t, 0, delim);
      }
    });
    rex::test::Join(c, thread_a);
    rex::test::Join(c, thread_b);

    CHECK(a->first == "a1");
    CHECK(b->first == "b1");
    CHECK(a->second == "a2");
    CHECK(b->second == "b2");
  });
}
