/**
 * @file        tests/unit/kernel/format_test.cpp
 * @brief       sprintf formatting stays inside its buffers and follows the retail CRT
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <bit>
#include <string>
#include <vector>

#include <rex/kernel/title_runtime.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

u32 PutText(Caller& c, const std::string& text) {
  return c.AllocString(text);
}

void Store8(Caller& c, u32 address, u8 value) {
  *StateOf(c.GetRuntime()).GetAddressSpace()->TranslateVirtual<u8*>(address) = value;
}

std::string ReadText(Caller& c, u32 address) {
  std::string text;
  while (const u8 byte = c.Load8(address + static_cast<u32>(text.size()))) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

u32 PutCountedString(Caller& c, u32 length, u32 buffer) {
  const u32 address = c.Alloc(8);
  c.Store32(address, (length << 16) | length);
  c.Store32(address + 4, buffer);
  return address;
}

u64 Bits(f64 value) {
  return std::bit_cast<u64>(value);
}

}  // namespace

REX_TEST_CASE("format: a float with a large precision prints every digit", "[kernel][format]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(1024);
    const u32 format = PutText(c, "%.600f");
    CHECK(c.Kernel("sprintf", {out, format, Bits(1.5)}) == 512 + 2);
    const std::string text = ReadText(c, out);
    CHECK(text.size() == 514);
    CHECK(text.rfind("1.5000", 0) == 0);
    CHECK(text.find_first_not_of('0', 3) == std::string::npos);
  });
}

REX_TEST_CASE("format: a huge float prints all its digits", "[kernel][format]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(2048);
    const u32 format = PutText(c, "%.100f");
    const i32 count = static_cast<i32>(c.Kernel("sprintf", {out, format, Bits(1e300)}));
    CHECK(count == 301 + 1 + 100);
    CHECK(ReadText(c, out).size() == 402);
  });
}

REX_TEST_CASE("format: an integer precision of 512 fills the whole digit buffer",
              "[kernel][format]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(1024);
    const u32 format = PutText(c, "%.512d");
    CHECK(c.Kernel("sprintf", {out, format, 5}) == 512);
    const std::string text = ReadText(c, out);
    CHECK(text == std::string(511, '0') + "5");
    const u32 huge = PutText(c, "%.5000d");
    CHECK(c.Kernel("sprintf", {out, huge, 5}) == 512);
  });
}

REX_TEST_CASE("format: %Z prints the counted string and consumes its argument",
              "[kernel][format]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(256);
    const u32 counted = PutCountedString(c, 5, PutText(c, "Hello"));
    CHECK(c.Kernel("sprintf", {out, PutText(c, "[%Z]%d"), counted, 7}) == 8);
    CHECK(ReadText(c, out) == "[Hello]7");
    CHECK(c.Kernel("sprintf", {out, PutText(c, "[%Z]%d"), 0, 7}) == 9);
    CHECK(ReadText(c, out) == "[(null)]7");
    CHECK(c.Kernel("sprintf", {out, PutText(c, "[%Z]"), PutCountedString(c, 3, 0)}) == 8);
    CHECK(ReadText(c, out) == "[(null)]");
    const u32 wide = c.Alloc(4);
    Store8(c, wide, 0);
    Store8(c, wide + 1, 'H');
    Store8(c, wide + 2, 0);
    Store8(c, wide + 3, 'i');
    CHECK(c.Kernel("sprintf", {out, PutText(c, "[%wZ]%d"), PutCountedString(c, 4, wide), 9}) == 5);
    CHECK(ReadText(c, out) == "[Hi]9");
  });
}

REX_TEST_CASE("format: an unknown conversion prints its character", "[kernel][format]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 out = c.Alloc(64);
    CHECK(static_cast<i32>(c.Kernel("sprintf", {out, PutText(c, "a%yb"), 1})) == 3);
    CHECK(ReadText(c, out) == "ayb");
  });
}
