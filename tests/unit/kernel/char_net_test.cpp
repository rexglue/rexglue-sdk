/**
 * @file        tests/unit/kernel/char_net_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kSocketError = 0xFFFFFFFF;
}  // namespace

TEST_CASE("net: a socket with an address family, type or protocol above 255 fails", "[char][net]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 valid = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(valid != kSocketError);
    CHECK(c.XAM("NetDll_closesocket", {0, valid}) == 0);
    CHECK(c.XAM("NetDll_socket", {0, 0x102, 1, 6}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == 0x273F);
    CHECK(c.XAM("NetDll_socket", {0, 2, 0x101, 6}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == 0x273C);
    CHECK(c.XAM("NetDll_socket", {0, 2, 1, 0x106}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == 0x273B);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}
