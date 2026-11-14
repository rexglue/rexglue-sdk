/**
 * @file        tests/unit/base/socket_wait_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <thread>

#include <rex/platform/socket.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

using rex::platform::SocketHandle;

struct DatagramSocket {
  DatagramSocket() {
#if REX_PLATFORM_WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    handle = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  }

  ~DatagramSocket() {
    rex::platform::SocketClose(handle);
#if REX_PLATFORM_WIN32
    WSACleanup();
#endif
  }

  SocketHandle handle;
};

}  // namespace

REX_TEST_CASE("WaitSockets returns when the prepared event is set", "[base][socket]") {
  DatagramSocket datagram;
  REQUIRE(datagram.handle != rex::platform::kInvalidSocket);
  auto event = rex::platform::Event::CreateManualResetEvent(false);
  REQUIRE(rex::platform::SocketPrepare(datagram.handle, event.get()) == 0);

  std::jthread setter([&event] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    event->Set();
  });
  const rex::platform::SocketWait wait{datagram.handle, event.get(),
                                       rex::platform::SocketReady::kRead};
  const auto start = std::chrono::steady_clock::now();
  CHECK(rex::platform::WaitSockets({&wait, 1}, false, std::chrono::seconds(5)) ==
        rex::platform::WaitResult::kSuccess);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(4));
}

REX_TEST_CASE("WaitSockets times out after the prepared event is reset", "[base][socket]") {
  DatagramSocket datagram;
  REQUIRE(datagram.handle != rex::platform::kInvalidSocket);
  auto event = rex::platform::Event::CreateManualResetEvent(false);
  REQUIRE(rex::platform::SocketPrepare(datagram.handle, event.get()) == 0);
  event->Set();
  event->Reset();
  const rex::platform::SocketWait wait{datagram.handle, event.get(),
                                       rex::platform::SocketReady::kRead};
  CHECK(rex::platform::WaitSockets({&wait, 1}, false, std::chrono::milliseconds(50)) ==
        rex::platform::WaitResult::kTimeout);
}
