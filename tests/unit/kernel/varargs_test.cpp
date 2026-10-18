/**
 * @file        tests/unit/kernel/varargs_test.cpp
 * @brief       Variadic arguments past r10 are read from the argument image slots
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <string>

#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kBufferBytes = 256;

std::string ReadBuffer(Caller& c, u32 address) {
  return std::string(c.GetRuntime().GetMemory()->TranslateVirtual<const char*>(address));
}
}  // namespace

REX_TEST_CASE("varargs: the 8th variadic argument reads the 9th slot", "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(kBufferBytes);
    const u32 format = c.String("%d %d %d %d %d %d %d %d");
    c.Kernel("sprintf", {buffer, format, 1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(ReadBuffer(c, buffer) == "1 2 3 4 5 6 7 8");
  });
}

REX_TEST_CASE("varargs: a 64 bit value in the 9th slot keeps its high word", "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(kBufferBytes);
    const u32 format = c.String("%d %d %d %d %d %d %I64d");
    c.Kernel("sprintf", {buffer, format, 1, 2, 3, 4, 5, 6, 0x1122334455667788});
    CHECK(ReadBuffer(c, buffer) == "1 2 3 4 5 6 1234605616436508552");
  });
}
