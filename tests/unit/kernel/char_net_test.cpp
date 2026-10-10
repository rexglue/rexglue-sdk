/**
 * @file        tests/unit/kernel/char_net_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kSocketError = 0xFFFFFFFF;
constexpr u32 kLoopback = 0x7F000001;

u32 LoopbackAddress(Caller& c, u32 port) {
  const u32 name = c.Alloc(16);
  c.Store32(name, 0x00020000 | port);
  c.Store32(name + 4, kLoopback);
  return name;
}
}  // namespace

REX_TEST_CASE("net: a socket with an address family, type or protocol above 255 fails",
              "[char][net]") {
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

REX_TEST_CASE("net: accept copies no more address than a sockaddr holds", "[char][net]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47321;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 client = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(listener != kSocketError);
    REQUIRE(client != kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    REQUIRE(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 address = c.Alloc(64);
    for (u32 offset = 0; offset < 64; offset += 4) {
      c.Store32(address + offset, 0xA5A5A5A5);
    }
    const u32 length = c.Alloc(4);
    c.Store32(length, 64);
    const u32 peer = c.XAM("NetDll_accept", {0, listener, address, length});
    REQUIRE(peer != kSocketError);
    CHECK(c.Load32(length) <= 16);
    for (u32 offset = 16; offset < 64; offset += 4) {
      INFO("offset " << offset);
      CHECK(c.Load32(address + offset) == 0xA5A5A5A5);
    }
    CHECK(c.XAM("NetDll_closesocket", {0, peer}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, client}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a length above the int range fails every socket call", "[char][net]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47322;
    constexpr u32 kDatagramPort = 47323;
    constexpr u32 kTooLong = 0x80000000;
    constexpr u32 kWSAEFAULT = 0x271E;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 client = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(listener != kSocketError);
    REQUIRE(client != kSocketError);
    REQUIRE(datagram != kSocketError);

    CHECK(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), kTooLong}) == kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    CHECK(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), kTooLong}) ==
          kSocketError);
    REQUIRE(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kDatagramPort), 16}) == 0);

    const u32 option = c.Alloc(4);
    c.Store32(option, 1);
    CHECK(c.XAM("NetDll_setsockopt", {0, client, 0xFFFF, 0x0004, option, 0xFFFFFFFF}) ==
          kSocketError);

    const u32 buffer = c.Alloc(16);
    CHECK(c.XAM("NetDll_send", {0, client, buffer, kTooLong, 0}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEFAULT);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_recv", {0, client, buffer, kTooLong, 0}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEFAULT);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_sendto", {0, datagram, buffer, kTooLong, 0,
                                  LoopbackAddress(c, kDatagramPort), 16}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEFAULT);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_recvfrom", {0, datagram, buffer, kTooLong, 0, 0, 0}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEFAULT);

    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, client}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}
