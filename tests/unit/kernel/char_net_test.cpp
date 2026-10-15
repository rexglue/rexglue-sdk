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

#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <rex/runtime/object_table.h>
#include <rex/runtime/socket_object.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kSocketError = 0xFFFFFFFF;
constexpr u32 kLoopback = 0x7F000001;
constexpr u32 kWSAEINTR = 0x2714;
constexpr u32 kWSAENOTSOCK = 0x2736;

bool NativeOpen(rex::platform::SocketHandle native) {
  int type = 0;
#if REX_PLATFORM_WIN32
  int length = sizeof(type);
  return getsockopt(static_cast<SOCKET>(native), SOL_SOCKET, SO_TYPE,
                    reinterpret_cast<char*>(&type), &length) == 0;
#else
  socklen_t length = sizeof(type);
  return getsockopt(static_cast<int>(native), SOL_SOCKET, SO_TYPE, &type, &length) == 0;
#endif
}

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

REX_TEST_CASE("net: closesocket wakes a blocked recv with WSAEINTR", "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47324;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 client = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(listener != kSocketError);
    REQUIRE(client != kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    REQUIRE(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 length = c.Alloc(4);
    c.Store32(length, 16);
    const u32 peer = c.XAM("NetDll_accept", {0, listener, c.Alloc(16), length});
    REQUIRE(peer != kSocketError);

    auto result = std::make_shared<std::promise<std::pair<u32, u32>>>();
    auto done = result->get_future();
    const u32 receiver = c.Spawn([client, result](Caller& r) {
      const u32 received = r.XAM("NetDll_recv", {0, client, r.Alloc(16), 16, 0});
      result->set_value({received, r.XAM("NetDll_WSAGetLastError")});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(c.XAM("NetDll_closesocket", {0, client}) == 0);
    CHECKED_ELSE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      FAIL_CHECK("the blocked recv did not return");
      return;
    }
    const auto [received, error] = done.get();
    CHECK(received == kSocketError);
    CHECK(error == kWSAEINTR);
    c.Kernel("NtClose", {receiver});
    CHECK(c.XAM("NetDll_closesocket", {0, peer}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: closesocket wakes a blocked recvfrom with WSAEINTR", "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47325;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kPort), 16}) == 0);

    auto result = std::make_shared<std::promise<std::pair<u32, u32>>>();
    auto done = result->get_future();
    const u32 receiver = c.Spawn([datagram, result](Caller& r) {
      const u32 received = r.XAM("NetDll_recvfrom", {0, datagram, r.Alloc(16), 16, 0, 0, 0});
      result->set_value({received, r.XAM("NetDll_WSAGetLastError")});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECKED_ELSE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      FAIL_CHECK("the blocked recvfrom did not return");
      return;
    }
    const auto [received, error] = done.get();
    CHECK(received == kSocketError);
    CHECK(error == kWSAEINTR);
    c.Kernel("NtClose", {receiver});
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: closesocket wakes a blocked accept with WSAEINTR", "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47326;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(listener != kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);

    auto result = std::make_shared<std::promise<std::pair<u32, u32>>>();
    auto done = result->get_future();
    const u32 acceptor = c.Spawn([listener, result](Caller& a) {
      const u32 length = a.Alloc(4);
      a.Store32(length, 16);
      const u32 accepted = a.XAM("NetDll_accept", {0, listener, a.Alloc(16), length});
      result->set_value({accepted, a.XAM("NetDll_WSAGetLastError")});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECKED_ELSE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      FAIL_CHECK("the blocked accept did not return");
      return;
    }
    const auto [accepted, error] = done.get();
    CHECK(accepted == kSocketError);
    CHECK(error == kWSAEINTR);
    c.Kernel("NtClose", {acceptor});
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: closesocket wakes a blocked select with WSAEINTR", "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47333;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kPort), 16}) == 0);

    auto result = std::make_shared<std::promise<std::pair<u32, u32>>>();
    auto done = result->get_future();
    const u32 selector = c.Spawn([datagram, result](Caller& s) {
      const u32 readable = s.Alloc(4 + 64 * 4);
      s.Store32(readable, 1);
      s.Store32(readable + 4, datagram);
      const u32 selected = s.XAM("NetDll_select", {0, 0, readable, 0, 0, 0});
      result->set_value({selected, s.XAM("NetDll_WSAGetLastError")});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECKED_ELSE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      FAIL_CHECK("the blocked select did not return");
      return;
    }
    const auto [selected, error] = done.get();
    CHECK(selected == kSocketError);
    CHECK(error == kWSAEINTR);
    c.Kernel("NtClose", {selector});
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: closesocket keeps the native socket open while a call holds the object",
              "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    auto pinned = c.GetRuntime().GetObjectTable().Get<rex::runtime::SocketObject>(datagram);
    REQUIRE(pinned);
    const rex::platform::SocketHandle native = pinned->socket.GetHandle();
    REQUIRE(NativeOpen(native));
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(NativeOpen(native));
    pinned = {};
    CHECK_FALSE(NativeOpen(native));
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a call through another handle of a closed socket fails with WSAENOTSOCK",
              "[char][net][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    const u32 copy_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtDuplicateObject", {datagram, copy_ptr, 0}) == 0);
    const u32 copy = c.Load32(copy_ptr);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_recv", {0, copy, c.Alloc(16), 16, 0}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAENOTSOCK);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_closesocket", {0, copy}) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAENOTSOCK);
    c.Kernel("NtClose", {copy});
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

namespace {

constexpr u32 kWSAEINVAL = 0x2726;
constexpr u32 kWSAEWOULDBLOCK = 0x2733;
constexpr u32 kWSAEMSGSIZE = 0x2738;
constexpr u32 kWSAEOPNOTSUPP = 0x273D;
constexpr u32 kWSAEADDRINUSE = 0x2740;
constexpr u32 kWSAENOTCONN = 0x2749;
constexpr u32 kWSAECONNREFUSED = 0x274D;
constexpr u32 kFIONBIO = 0x8004667E;

u32 LastErrorOf(Caller& c, std::string_view name, std::initializer_list<u64> args) {
  c.XAM("NetDll_WSASetLastError", {0});
  CHECK(c.XAM(name, args) == kSocketError);
  return c.XAM("NetDll_WSAGetLastError");
}

u32 Nonblocking(Caller& c, u32 socket) {
  const u32 enable = c.Alloc(4);
  c.Store32(enable, 1);
  REQUIRE(c.XAM("NetDll_ioctlsocket", {0, socket, kFIONBIO, enable}) == 0);
  return socket;
}

}  // namespace

REX_TEST_CASE("net: a recv with no data on a nonblocking socket fails with WSAEWOULDBLOCK",
              "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47327;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = Nonblocking(c, c.XAM("NetDll_socket", {0, 2, 2, 17}));
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 buffer = c.Alloc(16);
    CHECK(LastErrorOf(c, "NetDll_recv", {0, datagram, buffer, 16, 0}) == kWSAEWOULDBLOCK);
    CHECK(LastErrorOf(c, "NetDll_recvfrom", {0, datagram, buffer, 16, 0, 0, 0}) == kWSAEWOULDBLOCK);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: send and shutdown on an unconnected stream socket fail with WSAENOTCONN",
              "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(stream != kSocketError);
    CHECK(LastErrorOf(c, "NetDll_send", {0, stream, c.Alloc(16), 16, 0}) == kWSAENOTCONN);
    CHECK(LastErrorOf(c, "NetDll_shutdown", {0, stream, 2}) == kWSAENOTCONN);
    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a connect to a loopback port nobody listens on fails with WSAECONNREFUSED",
              "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47328;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(stream != kSocketError);
    CHECK(LastErrorOf(c, "NetDll_connect", {0, stream, LoopbackAddress(c, kPort), 16}) ==
          kWSAECONNREFUSED);
    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a bind to a port in use fails with WSAEADDRINUSE", "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47329;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 first = c.XAM("NetDll_socket", {0, 2, 2, 17});
    const u32 second = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(c.XAM("NetDll_bind", {0, first, LoopbackAddress(c, kPort), 16}) == 0);
    CHECK(LastErrorOf(c, "NetDll_bind", {0, second, LoopbackAddress(c, kPort), 16}) ==
          kWSAEADDRINUSE);
    CHECK(c.XAM("NetDll_closesocket", {0, second}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, first}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: listen on a datagram socket fails with WSAEOPNOTSUPP", "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47330;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kPort), 16}) == 0);
    CHECK(LastErrorOf(c, "NetDll_listen", {0, datagram, 1}) == kWSAEOPNOTSUPP);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: accept on a stream socket that does not listen fails with WSAEINVAL",
              "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47331;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(c.XAM("NetDll_bind", {0, stream, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 length = c.Alloc(4);
    c.Store32(length, 16);
    CHECK(LastErrorOf(c, "NetDll_accept", {0, stream, c.Alloc(16), length}) == kWSAEINVAL);
    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a datagram above the size limit fails with WSAEMSGSIZE", "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47332;
    constexpr u32 kTooBig = 70000;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    const u32 buffer = c.Alloc(kTooBig);
    CHECK(LastErrorOf(c, "NetDll_sendto",
                      {0, datagram, buffer, kTooBig, 0, LoopbackAddress(c, kPort), 16}) ==
          kWSAEMSGSIZE);
    const u32 buffers = c.Alloc(8);
    c.Store32(buffers, kTooBig);
    c.Store32(buffers + 4, buffer);
    CHECK(LastErrorOf(c, "NetDll_WSASendTo",
                      {0, datagram, buffers, 1, c.Alloc(4), 0, LoopbackAddress(c, kPort), 16, 0,
                       0}) == kWSAEMSGSIZE);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a send after the sending side shut down fails with WSAESHUTDOWN",
              "[char][net][error]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47334;
    constexpr u32 kWSAESHUTDOWN = 0x274A;
    constexpr u32 kSendSide = 1;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 client = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    REQUIRE(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_shutdown", {0, client, kSendSide}) == 0);
    CHECK(LastErrorOf(c, "NetDll_send", {0, client, c.Alloc(16), 16, 0}) == kWSAESHUTDOWN);
    CHECK(c.XAM("NetDll_closesocket", {0, client}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

namespace {

u32 Timeval(Caller& c, u32 seconds, u32 microseconds) {
  const u32 timeout = c.Alloc(8);
  c.Store32(timeout, seconds);
  c.Store32(timeout + 4, microseconds);
  return timeout;
}

u32 SocketSet(Caller& c, std::initializer_list<u32> sockets) {
  const u32 set = c.Alloc(4 + static_cast<u32>(sockets.size()) * 4);
  c.Store32(set, static_cast<u32>(sockets.size()));
  u32 entry = set + 4;
  for (const u32 socket : sockets) {
    c.Store32(entry, socket);
    entry += 4;
  }
  return set;
}

}  // namespace

REX_TEST_CASE("net: select takes a set of more than 64 sockets", "[char][net][select]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kCount = 65;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 set = c.Alloc(4 + kCount * 4);
    c.Store32(set, kCount);
    std::vector<u32> sockets;
    for (u32 i = 0; i < kCount; ++i) {
      const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
      REQUIRE(datagram != kSocketError);
      c.Store32(set + 4 + i * 4, datagram);
      sockets.push_back(datagram);
    }
    CHECK(c.XAM("NetDll_select", {0, 0, 0, set, 0, Timeval(c, 0, 0)}) == kCount);
    CHECK(c.Load32(set) == kCount);
    for (const u32 datagram : sockets) {
      CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    }
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: select reports a socket with a datagram waiting as readable",
              "[char][net][select]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47335;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 receiver = c.XAM("NetDll_socket", {0, 2, 2, 17});
    const u32 sender = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(c.XAM("NetDll_bind", {0, receiver, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_sendto", {0, sender, c.Alloc(8), 8, 0, LoopbackAddress(c, kPort), 16}) ==
            8);
    const u32 set = SocketSet(c, {receiver});
    CHECK(c.XAM("NetDll_select", {0, 0, set, 0, 0, Timeval(c, 1, 0)}) == 1);
    CHECK(c.Load32(set) == 1);
    CHECK(c.Load32(set + 4) == receiver);
    CHECK(c.XAM("NetDll_closesocket", {0, sender}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, receiver}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: select counts a socket once for each set it is ready in",
              "[char][net][select]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47360;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 receiver = c.XAM("NetDll_socket", {0, 2, 2, 17});
    const u32 sender = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(c.XAM("NetDll_bind", {0, receiver, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_sendto", {0, sender, c.Alloc(8), 8, 0, LoopbackAddress(c, kPort), 16}) ==
            8);
    const u32 readable = SocketSet(c, {receiver});
    const u32 writable = SocketSet(c, {receiver});
    CHECK(c.XAM("NetDll_select", {0, 0, readable, writable, 0, Timeval(c, 1, 0)}) == 2);
    CHECK(c.Load32(readable) + c.Load32(writable) == 2);
    CHECK(c.XAM("NetDll_closesocket", {0, sender}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, receiver}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

namespace {

void CheckSetEndsAt(u32 marker) {
  KernelFixture fixture;
  fixture.RunOnGuestThread([marker](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 first = c.XAM("NetDll_socket", {0, 2, 2, 17});
    const u32 second = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(first != kSocketError);
    REQUIRE(second != kSocketError);
    const u32 set = SocketSet(c, {first, marker, second});
    CHECK(c.XAM("NetDll_select", {0, 0, 0, set, 0, Timeval(c, 0, 0)}) == 1);
    CHECK(c.Load32(set) == 1);
    CHECK(c.Load32(set + 4) == first);
    CHECK(c.XAM("NetDll_closesocket", {0, second}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, first}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

}  // namespace

REX_TEST_CASE("net: select ends a set at the invalid socket marker", "[char][net][select]") {
  CheckSetEndsAt(0xFFFFFFFF);
}

REX_TEST_CASE("net: select ends a set at an entry that is not a socket", "[char][net][select]") {
  CheckSetEndsAt(0x12345678);
}

REX_TEST_CASE("net: select with no socket in any set fails with WSAEINVAL", "[char][net][select]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    CHECK(LastErrorOf(c, "NetDll_select", {0, 0, 0, 0, 0, Timeval(c, 0, 0)}) == kWSAEINVAL);
    CHECK(LastErrorOf(c, "NetDll_select", {0, 0, SocketSet(c, {}), 0, 0, Timeval(c, 0, 0)}) ==
          kWSAEINVAL);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: select with a timeout and no ready socket returns 0 at the timeout",
              "[char][net][select]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47336;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(c.XAM("NetDll_bind", {0, datagram, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 set = SocketSet(c, {datagram});
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.XAM("NetDll_select", {0, 0, set, 0, 0, Timeval(c, 0, 100000)}) == 0);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(90));
    CHECK(c.Load32(set) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("net: select takes a socket whose host descriptor is above 1023",
              "[char][net][select]") {
  rlimit limit{};
  REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
  constexpr rlim_t kHighFD = 1100;
  if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max <= kHighFD + 16) {
    SKIP("the descriptor hard limit is " << limit.rlim_max);
  }
  const rlimit saved = limit;
  limit.rlim_cur = limit.rlim_max;
  REQUIRE(setrlimit(RLIMIT_NOFILE, &limit) == 0);
  std::vector<int> fillers;
  for (int fd = dup(STDIN_FILENO); fd != -1 && fd < static_cast<int>(kHighFD);
       fd = dup(STDIN_FILENO)) {
    fillers.push_back(fd);
  }
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    auto pinned = c.GetRuntime().GetObjectTable().Get<rex::runtime::SocketObject>(datagram);
    REQUIRE(pinned);
    CHECK(pinned->socket.GetHandle() >= 1024);
    pinned = {};
    const u32 set = SocketSet(c, {datagram});
    CHECK(c.XAM("NetDll_select", {0, 0, 0, set, 0, Timeval(c, 0, 0)}) == 1);
    CHECK(c.Load32(set) == 1);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
  for (const int fd : fillers) {
    close(fd);
  }
  setrlimit(RLIMIT_NOFILE, &saved);
}
#endif

namespace {

u32 SingleBuffer(Caller& c, u32 length) {
  const u32 buffers = c.Alloc(8);
  c.Store32(buffers, length);
  c.Store32(buffers + 4, c.Alloc(length));
  return buffers;
}

}  // namespace

REX_TEST_CASE("net: an overlapped WSASendTo completes with its byte count and event",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47337;
    constexpr u32 kLength = 8;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    const u32 event = c.XAM("NetDll_WSACreateEvent");
    REQUIRE(event != 0);
    const u32 overlapped = c.Alloc(20);
    c.Store32(overlapped + 16, event);
    const u32 sent = c.Alloc(4);
    c.Store32(sent, 0xFFFFFFFF);
    CHECK(c.XAM("NetDll_WSASendTo", {0, datagram, SingleBuffer(c, kLength), 1, sent, 0,
                                     LoopbackAddress(c, kPort), 16, overlapped, 0}) == 0);
    CHECK(c.Load32(sent) == kLength);
    const u32 events = c.Alloc(4);
    c.Store32(events, event);
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 0, 0}) == 0);
    const u32 transferred = c.Alloc(4);
    const u32 flags = c.Alloc(4);
    c.Store32(flags, 0xFFFFFFFF);
    CHECK(c.XAM("NetDll_WSAGetOverlappedResult",
                {0, datagram, overlapped, transferred, 0, flags}) == 1);
    CHECK(c.Load32(transferred) == kLength);
    CHECK(c.Load32(flags) == 0);
    CHECK(c.XAM("NetDll_WSACloseEvent", {event}) == 1);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a WSASendTo without an overlapped reports its byte count",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47338;
    constexpr u32 kLength = 12;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    const u32 sent = c.Alloc(4);
    c.Store32(sent, 0xFFFFFFFF);
    CHECK(c.XAM("NetDll_WSASendTo", {0, datagram, SingleBuffer(c, kLength), 1, sent, 0,
                                     LoopbackAddress(c, kPort), 16, 0, 0}) == 0);
    CHECK(c.Load32(sent) == kLength);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a WSASendTo with a completion routine still sends", "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47339;
    constexpr u32 kLength = 8;
    constexpr u32 kRoutine = 0x82000000;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);
    const u32 event = c.XAM("NetDll_WSACreateEvent");
    REQUIRE(event != 0);
    const u32 overlapped = c.Alloc(20);
    c.Store32(overlapped + 16, event);
    const u32 sent = c.Alloc(4);
    CHECK(c.XAM("NetDll_WSASendTo", {0, datagram, SingleBuffer(c, kLength), 1, sent, 0,
                                     LoopbackAddress(c, kPort), 16, overlapped, kRoutine}) == 0);
    CHECK(c.Load32(sent) == kLength);
    CHECK(c.Load32(overlapped) == 0);
    CHECK(c.Load32(overlapped + 4) == kLength);
    const u32 events = c.Alloc(4);
    c.Store32(events, event);
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 0, 0}) == 0);
    CHECK(c.XAM("NetDll_WSACloseEvent", {event}) == 1);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: WSAGetOverlappedResult of a failed overlapped send returns its error",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47340;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(stream != kSocketError);
    const u32 event = c.XAM("NetDll_WSACreateEvent");
    REQUIRE(event != 0);
    const u32 overlapped = c.Alloc(20);
    c.Store32(overlapped + 16, event);
    const u32 error = LastErrorOf(c, "NetDll_WSASendTo",
                                  {0, stream, SingleBuffer(c, 8), 1, c.Alloc(4), 0,
                                   LoopbackAddress(c, kPort), 16, overlapped, 0});
    REQUIRE(error != 0);
    CHECK(c.Load32(overlapped) == error);
    const u32 events = c.Alloc(4);
    c.Store32(events, event);
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 0, 0}) == 0);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_WSAGetOverlappedResult",
                {0, stream, overlapped, c.Alloc(4), 0, c.Alloc(4)}) == 0);
    CHECK(c.XAM("NetDll_WSAGetLastError") == error);
    CHECK(c.XAM("NetDll_WSACloseEvent", {event}) == 1);
    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

namespace {

constexpr u32 kWSAIOIncomplete = 996;
constexpr u32 kWSAIOPending = 997;
constexpr u32 kWSAECANCELLED = 0x2777;
constexpr u32 kUntouched = 0xCDCDCDCD;

struct Receive {
  u32 socket = 0;
  u32 buffers = 0;
  u32 bytes = 0;
  u32 flags = 0;
  u32 from = 0;
  u32 from_len = 0;
  u32 overlapped = 0;
  u32 event = 0;
};

Receive PrepareReceive(Caller& c, u32 port, u32 from_len, bool overlapped) {
  Receive r;
  r.socket = c.XAM("NetDll_socket", {0, 2, 2, 17});
  REQUIRE(r.socket != kSocketError);
  REQUIRE(c.XAM("NetDll_bind", {0, r.socket, LoopbackAddress(c, port), 16}) == 0);
  r.buffers = SingleBuffer(c, 16);
  for (u32 offset = 0; offset < 16; offset += 4) {
    c.Store32(c.Load32(r.buffers + 4) + offset, kUntouched);
  }
  r.bytes = c.Alloc(4);
  c.Store32(r.bytes, kUntouched);
  r.flags = c.Alloc(4);
  c.Store32(r.flags, kUntouched);
  r.from = c.Alloc(16);
  for (u32 offset = 0; offset < 16; offset += 4) {
    c.Store32(r.from + offset, kUntouched);
  }
  r.from_len = c.Alloc(4);
  c.Store32(r.from_len, from_len);
  if (overlapped) {
    r.event = c.XAM("NetDll_WSACreateEvent");
    REQUIRE(r.event != 0);
    r.overlapped = c.Alloc(20);
    c.Store32(r.overlapped + 16, r.event);
  }
  return r;
}

u32 StartReceive(Caller& c, const Receive& r) {
  return c.XAM("NetDll_WSARecvFrom",
               {0, r.socket, r.buffers, 1, r.bytes, r.flags, r.from, r.from_len, r.overlapped, 0});
}

u32 StartPendingReceive(Caller& c, const Receive& r) {
  return LastErrorOf(
      c, "NetDll_WSARecvFrom",
      {0, r.socket, r.buffers, 1, r.bytes, r.flags, r.from, r.from_len, r.overlapped, 0});
}

void SendDatagram(Caller& c, u32 port) {
  const u32 sender = c.XAM("NetDll_socket", {0, 2, 2, 17});
  REQUIRE(sender != kSocketError);
  const u32 payload = c.Alloc(8);
  c.Store32(payload, 0x11223344);
  c.Store32(payload + 4, 0x55667788);
  REQUIRE(c.XAM("NetDll_sendto", {0, sender, payload, 8, 0, LoopbackAddress(c, port), 16}) == 8);
  CHECK(c.XAM("NetDll_closesocket", {0, sender}) == 0);
}

void WaitReadable(Caller& c, u32 socket) {
  REQUIRE(c.XAM("NetDll_select", {0, 0, SocketSet(c, {socket}), 0, 0, Timeval(c, 5, 0)}) == 1);
}

bool Signaled(Caller& c, u32 event, u32 timeout_ms) {
  const u32 events = c.Alloc(4);
  c.Store32(events, event);
  return c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, timeout_ms, 0}) == 0;
}

void CheckPayload(Caller& c, const Receive& r) {
  const u32 data = c.Load32(r.buffers + 4);
  CHECK(c.Load32(data) == 0x11223344);
  CHECK(c.Load32(data + 4) == 0x55667788);
  CHECK(c.Load32(data + 8) == kUntouched);
}

void CheckBufferUntouched(Caller& c, const Receive& r) {
  const u32 data = c.Load32(r.buffers + 4);
  for (u32 offset = 0; offset < 16; offset += 4) {
    CHECK(c.Load32(data + offset) == kUntouched);
  }
}

void CheckLoopbackFrom(Caller& c, const Receive& r) {
  CHECK(c.Load32(r.from) >> 16 == 2);
  CHECK(c.Load32(r.from + 4) == kLoopback);
  CHECK(c.Load32(r.from_len) == 16);
}

void Finish(Caller& c, const Receive& r) {
  if (r.event) {
    CHECK(c.XAM("NetDll_WSACloseEvent", {r.event}) == 1);
  }
  CHECK(c.XAM("NetDll_closesocket", {0, r.socket}) == 0);
}

}  // namespace

REX_TEST_CASE("net: an overlapped WSARecvFrom with a datagram waiting completes at once",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47341;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    SendDatagram(c, kPort);
    WaitReadable(c, r.socket);
    CHECK(StartReceive(c, r) == 0);
    CHECK(c.Load32(r.bytes) == 8);
    CHECK(c.Load32(r.flags) == 0);
    CHECK(c.LoadAcquire32(r.overlapped) == 0);
    CHECK(c.Load32(r.overlapped + 4) == 8);
    CHECK(Signaled(c, r.event, 0));
    CheckPayload(c, r);
    CheckLoopbackFrom(c, r);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a pending WSARecvFrom completes when a datagram arrives",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47342;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    CHECK(c.LoadAcquire32(r.overlapped) == kWSAIOIncomplete);
    const u32 transferred = c.Alloc(4);
    const u32 flags = c.Alloc(4);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_WSAGetOverlappedResult",
                {0, r.socket, r.overlapped, transferred, 0, flags}) == 0);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAIOIncomplete);
    SendDatagram(c, kPort);
    CHECK(c.XAM("NetDll_WSAGetOverlappedResult",
                {0, r.socket, r.overlapped, transferred, 1, flags}) == 1);
    CHECK(c.Load32(transferred) == 8);
    CHECK(c.Load32(flags) == 0);
    CHECK(Signaled(c, r.event, 0));
    CheckPayload(c, r);
    CheckLoopbackFrom(c, r);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a pending WSARecvFrom leaves the byte count and flags of its call untouched",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47350;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    SendDatagram(c, kPort);
    CHECK(Signaled(c, r.event, 5000));
    CheckPayload(c, r);
    CHECK(c.Load32(r.bytes) == kUntouched);
    CHECK(c.Load32(r.flags) == kUntouched);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: WSACancelOverlappedIO ends a pending WSARecvFrom with WSAECANCELLED",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47343;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    CHECK(c.XAM("NetDll_WSACancelOverlappedIO", {0, r.socket}) == 0);
    CHECK(Signaled(c, r.event, 0));
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(c.XAM("NetDll_WSAGetOverlappedResult",
                {0, r.socket, r.overlapped, c.Alloc(4), 0, c.Alloc(4)}) == 0);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAECANCELLED);
    SendDatagram(c, kPort);
    WaitReadable(c, r.socket);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(c.LoadAcquire32(r.overlapped) == kWSAECANCELLED);
    CheckBufferUntouched(c, r);
    CHECK(c.Load32(r.from) == kUntouched);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a WSARecvFrom without an overlapped waits for a datagram",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47344;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, false);
    const u32 sender = c.Spawn([](Caller& s) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      SendDatagram(s, kPort);
    });
    CHECK(StartReceive(c, r) == 0);
    CHECK(c.Load32(r.bytes) == 8);
    CHECK(c.Load32(r.flags) == 0);
    CheckPayload(c, r);
    CheckLoopbackFrom(c, r);
    c.Kernel("NtClose", {sender});
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: a second pending WSARecvFrom on a socket fails with WSAEWOULDBLOCK",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47345;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    const u32 second = c.Alloc(20);
    c.Store32(second + 16, r.event);
    CHECK(LastErrorOf(c, "NetDll_WSARecvFrom",
                      {0, r.socket, r.buffers, 1, r.bytes, r.flags, r.from, r.from_len, second,
                       0}) == kWSAEWOULDBLOCK);
    CHECK(c.XAM("NetDll_WSACancelOverlappedIO", {0, r.socket}) == 0);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: closesocket ends a pending WSARecvFrom with WSAECANCELLED",
              "[char][net][overlapped][close]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47346;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    CHECK(c.XAM("NetDll_closesocket", {0, r.socket}) == 0);
    CHECK(Signaled(c, r.event, 0));
    CHECK(c.LoadAcquire32(r.overlapped) == kWSAECANCELLED);
    SendDatagram(c, kPort);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(c.LoadAcquire32(r.overlapped) == kWSAECANCELLED);
    CheckBufferUntouched(c, r);
    CHECK(c.Load32(r.from) == kUntouched);
    CHECK(c.XAM("NetDll_WSACloseEvent", {r.event}) == 1);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: title termination ends a pending WSARecvFrom before it returns",
              "[char][net][overlapped][terminate]") {
  KernelFixture fixture;
  u32 overlapped = 0;
  fixture.RunOnGuestThread([&overlapped](Caller& c) {
    constexpr u32 kPort = 47347;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 16, true);
    CHECK(StartPendingReceive(c, r) == kWSAIOPending);
    overlapped = r.overlapped;
  });
  REQUIRE(overlapped != 0);
  fixture.GetRuntime().TerminateTitle();
  auto* internal = fixture.GetRuntime().GetMemory()->TranslateVirtual<rex::be_u32*>(overlapped);
  CHECK(*internal == kWSAECANCELLED);
}

REX_TEST_CASE("net: WSARecvFrom reads the address length from its 8th argument",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47348;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 32, false);
    SendDatagram(c, kPort);
    WaitReadable(c, r.socket);
    CHECK(StartReceive(c, r) == 0);
    CheckLoopbackFrom(c, r);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: WSARecvFrom writes no more of the address than the length allows",
              "[char][net][overlapped]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47349;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const Receive r = PrepareReceive(c, kPort, 8, false);
    SendDatagram(c, kPort);
    WaitReadable(c, r.socket);
    CHECK(StartReceive(c, r) == 0);
    CHECK(c.Load32(r.from) >> 16 == 2);
    CHECK(c.Load32(r.from + 4) == kLoopback);
    CHECK(c.Load32(r.from + 8) == kUntouched);
    CHECK(c.Load32(r.from + 12) == kUntouched);
    CHECK(c.Load32(r.from_len) == 16);
    Finish(c, r);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

namespace {

constexpr u32 kSolSocket = 0xFFFF;
constexpr u32 kSoReuseAddr = 0x0004;
constexpr u32 kSoBroadcast = 0x0020;
constexpr u32 kSoLinger = 0x0080;
constexpr u32 kSoSndBuf = 0x1001;
constexpr u32 kSoRcvBuf = 0x1002;
constexpr u32 kSoSndTimeo = 0x1005;
constexpr u32 kSoRcvTimeo = 0x1006;
constexpr u32 kSoType = 0x1008;
constexpr u32 kProtocolTCP = 6;
constexpr u32 kTCPNoDelay = 1;
constexpr u32 kWSAENOPROTOOPT = 0x277A;

void SetHostNoDelay(rex::platform::SocketHandle native, int value) {
#if REX_PLATFORM_WIN32
  setsockopt(static_cast<SOCKET>(native), IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&value), sizeof(value));
#else
  setsockopt(static_cast<int>(native), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
}

int GetHostNoDelay(rex::platform::SocketHandle native) {
  int value = -1;
#if REX_PLATFORM_WIN32
  int length = sizeof(value);
  getsockopt(static_cast<SOCKET>(native), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value),
             &length);
#else
  socklen_t length = sizeof(value);
  getsockopt(static_cast<int>(native), IPPROTO_TCP, TCP_NODELAY, &value, &length);
#endif
  return value != 0 ? 1 : 0;
}

u32 SetOption(Caller& c, u32 socket, u32 level, u32 name, u32 value, u32 length = 4) {
  const u32 option = c.Alloc(8);
  c.Store32(option, value);
  return c.XAM("NetDll_setsockopt", {0, socket, level, name, option, length});
}

u32 GetOption(Caller& c, u32 socket, u32 level, u32 name, u32* value, u32 length = 4) {
  const u32 option = c.Alloc(8);
  const u32 option_length = c.Alloc(4);
  c.Store32(option_length, length);
  const u32 result = c.XAM("NetDll_getsockopt", {0, socket, level, name, option, option_length});
  *value = c.Load32(option);
  return result;
}

}  // namespace

REX_TEST_CASE("net: setsockopt reads 4 byte values as big endian integers",
              "[kernel][net][sockopt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    REQUIRE(datagram != kSocketError);

    REQUIRE(SetOption(c, datagram, kSolSocket, kSoRcvBuf, 0x20000) == 0);
    REQUIRE(SetOption(c, datagram, kSolSocket, kSoSndBuf, 0x20000) == 0);
    u32 value = 0;
    REQUIRE(GetOption(c, datagram, kSolSocket, kSoRcvBuf, &value) == 0);
    CHECK(value >= 0x20000);
    CHECK(value < 0x1000000);
    REQUIRE(GetOption(c, datagram, kSolSocket, kSoSndBuf, &value) == 0);
    CHECK(value >= 0x20000);
    CHECK(value < 0x1000000);

    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: boolean options round trip through setsockopt and getsockopt",
              "[kernel][net][sockopt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 datagram = c.XAM("NetDll_socket", {0, 2, 2, 17});
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(datagram != kSocketError);
    REQUIRE(stream != kSocketError);

    u32 value = 0xFFFFFFFF;
    REQUIRE(GetOption(c, datagram, kSolSocket, kSoBroadcast, &value) == 0);
    CHECK(value == 0);
    REQUIRE(SetOption(c, datagram, kSolSocket, kSoBroadcast, 1) == 0);
    REQUIRE(GetOption(c, datagram, kSolSocket, kSoBroadcast, &value) == 0);
    CHECK(value != 0);
    REQUIRE(SetOption(c, datagram, kSolSocket, kSoBroadcast, 0) == 0);
    REQUIRE(GetOption(c, datagram, kSolSocket, kSoBroadcast, &value) == 0);
    CHECK(value == 0);

    REQUIRE(SetOption(c, stream, kSolSocket, kSoReuseAddr, 1) == 0);
    REQUIRE(GetOption(c, stream, kSolSocket, kSoReuseAddr, &value) == 0);
    CHECK(value != 0);

    value = 0;
    REQUIRE(GetOption(c, stream, kProtocolTCP, kTCPNoDelay, &value) == 0);
    CHECK(value == 1);
    REQUIRE(SetOption(c, stream, kProtocolTCP, kTCPNoDelay, 0) == 0);
    value = 0;
    REQUIRE(GetOption(c, stream, kProtocolTCP, kTCPNoDelay, &value) == 0);
    CHECK(value == 1);
    REQUIRE(SetOption(c, stream, kProtocolTCP, kTCPNoDelay, 1) == 0);
    REQUIRE(GetOption(c, stream, kProtocolTCP, kTCPNoDelay, &value) == 0);
    CHECK(value != 0);

    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, datagram}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: linger and timeouts convert between title and host layouts",
              "[kernel][net][sockopt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(stream != kSocketError);

    REQUIRE(SetOption(c, stream, kSolSocket, kSoLinger, (1u << 16) | 5u) == 0);
    u32 value = 0;
    REQUIRE(GetOption(c, stream, kSolSocket, kSoLinger, &value, 4) == 0);
    CHECK(value == ((1u << 16) | 5u));

    REQUIRE(SetOption(c, stream, kSolSocket, kSoRcvTimeo, 1500) == 0);
    REQUIRE(SetOption(c, stream, kSolSocket, kSoSndTimeo, 2500) == 0);
    REQUIRE(GetOption(c, stream, kSolSocket, kSoRcvTimeo, &value) == 0);
    CHECK(value == 1500);
    REQUIRE(GetOption(c, stream, kSolSocket, kSoSndTimeo, &value) == 0);
    CHECK(value == 2500);

    REQUIRE(GetOption(c, stream, kSolSocket, kSoType, &value) == 0);
    CHECK(value == 1);

    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: unsupported options and short buffers fail with the Winsock errors",
              "[kernel][net][sockopt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 stream = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(stream != kSocketError);

    u32 value = 0;
    CHECK(SetOption(c, stream, kSolSocket, 0x7777, 1) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAENOPROTOOPT);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(GetOption(c, stream, kSolSocket, 0x7777, &value) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAENOPROTOOPT);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(GetOption(c, stream, 0x1234, kSoType, &value) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEINVAL);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(SetOption(c, stream, 0x1234, kSoType, 1) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEINVAL);
    c.XAM("NetDll_WSASetLastError", {0});
    CHECK(GetOption(c, stream, kProtocolTCP, 0x7777, &value) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAENOPROTOOPT);
    c.XAM("NetDll_WSASetLastError", {0});

    constexpr u32 kWSAEFAULT = 0x271E;
    CHECK(GetOption(c, stream, kSolSocket, kSoRcvBuf, &value, 2) == kSocketError);
    CHECK(c.XAM("NetDll_WSAGetLastError") == kWSAEFAULT);

    CHECK(c.XAM("NetDll_closesocket", {0, stream}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}

REX_TEST_CASE("net: an accepted stream socket disables Nagle on the host",
              "[kernel][net][sockopt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kPort = 47361;
    REQUIRE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0);
    const u32 listener = c.XAM("NetDll_socket", {0, 2, 1, 6});
    const u32 client = c.XAM("NetDll_socket", {0, 2, 1, 6});
    REQUIRE(listener != kSocketError);
    REQUIRE(client != kSocketError);
    auto pinned = c.GetRuntime().GetObjectTable().Get<rex::runtime::SocketObject>(listener);
    REQUIRE(pinned);
    SetHostNoDelay(pinned->socket.GetHandle(), 0);
    REQUIRE(GetHostNoDelay(pinned->socket.GetHandle()) == 0);
    pinned = {};
    REQUIRE(c.XAM("NetDll_bind", {0, listener, LoopbackAddress(c, kPort), 16}) == 0);
    REQUIRE(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    REQUIRE(c.XAM("NetDll_connect", {0, client, LoopbackAddress(c, kPort), 16}) == 0);
    const u32 length = c.Alloc(4);
    c.Store32(length, 16);
    const u32 peer = c.XAM("NetDll_accept", {0, listener, c.Alloc(16), length});
    REQUIRE(peer != kSocketError);
    auto accepted = c.GetRuntime().GetObjectTable().Get<rex::runtime::SocketObject>(peer);
    REQUIRE(accepted);
    CHECK(GetHostNoDelay(accepted->socket.GetHandle()) == 1);
    u32 value = 0;
    REQUIRE(GetOption(c, peer, kProtocolTCP, kTCPNoDelay, &value) == 0);
    CHECK(value == 1);
    accepted = {};
    CHECK(c.XAM("NetDll_closesocket", {0, peer}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, client}) == 0);
    CHECK(c.XAM("NetDll_closesocket", {0, listener}) == 0);
    CHECK(c.XAM("NetDll_WSACleanup", {0}) == 0);
  });
}
