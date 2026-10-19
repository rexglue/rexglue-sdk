/**
 * @file        tests/unit/core/socket_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <utility>

#include <rex/net/socket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

using rex::platform::Socket;
using rex::platform::SocketHandle;

struct Winsock {
#if REX_PLATFORM_WIN32
  Winsock() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~Winsock() { WSACleanup(); }
#endif
};

bool IsOpen(SocketHandle handle) {
  int type = 0;
#if REX_PLATFORM_WIN32
  int length = sizeof(type);
  return getsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_TYPE,
                    reinterpret_cast<char*>(&type), &length) == 0;
#else
  socklen_t length = sizeof(type);
  return getsockopt(static_cast<int>(handle), SOL_SOCKET, SO_TYPE, &type, &length) == 0;
#endif
}

}  // namespace

REX_TEST_CASE("A moved-from Socket does not close the native socket", "[core][socket]") {
  [[maybe_unused]] Winsock winsock;
  Socket target;
  SocketHandle handle = rex::platform::kInvalidSocket;
  {
    Socket source = Socket::Create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(source.IsValid());
    handle = source.GetHandle();
    target = std::move(source);
  }
  CHECK(target.IsValid());
  CHECK(target.GetHandle() == handle);
  CHECK(IsOpen(handle));
}

REX_TEST_CASE("A default Socket holds no native socket", "[core][socket]") {
  CHECK_FALSE(Socket{}.IsValid());
}

REX_TEST_CASE("Destroying a Socket closes the native socket", "[core][socket]") {
  [[maybe_unused]] Winsock winsock;
  SocketHandle handle = rex::platform::kInvalidSocket;
  {
    Socket socket = Socket::Create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(socket.IsValid());
    handle = socket.GetHandle();
    REQUIRE(IsOpen(handle));
  }
  CHECK_FALSE(IsOpen(handle));
}
