/**
 * @file        core/net_socket_test.cpp
 * @brief       Tests for the guest (Winsock) to host socket translation
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

#include <rex/net/socket.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net = rex::net;
namespace wsa = rex::net::wsa;

namespace {

#if REX_PLATFORM_WIN32
struct WinsockInit {
  WinsockInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockInit() { WSACleanup(); }
};
using HostSocket = SOCKET;
using HostLength = int;
#else
struct WinsockInit {};
using HostSocket = int;
using HostLength = socklen_t;
#endif

// A UDP socket on 127.0.0.1 with a port the system picks.
struct LoopbackUdp {
  net::SocketHandle handle = net::kInvalidSocket;
  uint16_t port = 0;

  LoopbackUdp() {
    handle = net::socket_open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == net::kInvalidSocket) {
      return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(net::kIpv4Loopback);
    bind(static_cast<HostSocket>(handle), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    HostLength len = sizeof(addr);
    getsockname(static_cast<HostSocket>(handle), reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
  }
  ~LoopbackUdp() {
    if (handle != net::kInvalidSocket) {
      net::socket_close(handle);
    }
  }
};

}  // namespace

TEST_CASE("Winsock socket options map to the host's numbers", "[net]") {
  auto broadcast = net::translate_socket_option(wsa::kSolSocket, wsa::kSoBroadcast);
  CHECK(broadcast.kind == net::OptionKind::kInt);
  CHECK(broadcast.level == SOL_SOCKET);
  CHECK(broadcast.name == SO_BROADCAST);

  auto reuse = net::translate_socket_option(0xFFFF, 0x0004);
  CHECK(reuse.kind == net::OptionKind::kInt);
  CHECK(reuse.level == SOL_SOCKET);
  CHECK(reuse.name == SO_REUSEADDR);

  auto rcvbuf = net::translate_socket_option(0xFFFF, 0x1002);
  CHECK(rcvbuf.level == SOL_SOCKET);
  CHECK(rcvbuf.name == SO_RCVBUF);

  auto nodelay = net::translate_socket_option(6, 1);
  CHECK(nodelay.kind == net::OptionKind::kInt);
  CHECK(nodelay.level == IPPROTO_TCP);
  CHECK(nodelay.name == TCP_NODELAY);

  CHECK(net::translate_socket_option(0xFFFF, 0x0080).kind == net::OptionKind::kLinger);
  CHECK(net::translate_socket_option(0xFFFF, ~0x0080u).kind == net::OptionKind::kDontLinger);
  CHECK(net::translate_socket_option(0xFFFF, 0x1006).kind == net::OptionKind::kTimeoutMs);

  // The console's encryption switches are accepted and ignored.
  CHECK(net::translate_socket_option(0xFFFF, 0x5801).kind == net::OptionKind::kIgnored);
  CHECK(net::translate_socket_option(0xFFFF, 0x5802).kind == net::OptionKind::kIgnored);

  // Anything else is refused, never passed through with guest numbers.
  CHECK(net::translate_socket_option(0xFFFF, 0x1234).kind == net::OptionKind::kUnsupported);
  CHECK(net::translate_socket_option(0, 4).kind == net::OptionKind::kUnsupported);
  CHECK(net::translate_socket_option(1, 6).kind == net::OptionKind::kUnsupported);
}

TEST_CASE("Winsock message flags map to the host's", "[net]") {
  int host = -1;
  CHECK(net::translate_message_flags(0, &host));
  CHECK(host == 0);
  CHECK(net::translate_message_flags(wsa::kMsgPeek, &host));
  CHECK(host == MSG_PEEK);
  CHECK(net::translate_message_flags(wsa::kMsgOob | wsa::kMsgDontRoute, &host));
  CHECK(host == (MSG_OOB | MSG_DONTROUTE));
  CHECK_FALSE(net::translate_message_flags(0x8, &host));  // MSG_WAITALL: not supported
}

#if !REX_PLATFORM_WIN32
TEST_CASE("errno values become Winsock error codes", "[net]") {
  CHECK(net::wsa_error_from_errno(0) == 0);
  CHECK(net::wsa_error_from_errno(EAGAIN) == 10035);
  CHECK(net::wsa_error_from_errno(EWOULDBLOCK) == 10035);
  CHECK(net::wsa_error_from_errno(EMSGSIZE) == 10040);
  CHECK(net::wsa_error_from_errno(ENOPROTOOPT) == 10042);
  CHECK(net::wsa_error_from_errno(EADDRINUSE) == 10048);
  CHECK(net::wsa_error_from_errno(EADDRNOTAVAIL) == 10049);
  CHECK(net::wsa_error_from_errno(ENETUNREACH) == 10051);
  CHECK(net::wsa_error_from_errno(ECONNRESET) == 10054);
  CHECK(net::wsa_error_from_errno(EHOSTUNREACH) == 10065);
  CHECK(net::wsa_error_from_errno(EACCES) == 10013);
  CHECK(net::wsa_error_from_errno(EINVAL) == 10022);
}
#endif

TEST_CASE("IPv4 text and address classes", "[net]") {
  uint32_t addr = 0;
  CHECK(net::parse_ipv4("192.168.1.20", &addr));
  CHECK(addr == 0xC0A80114);
  CHECK(net::format_ipv4(addr) == "192.168.1.20");
  CHECK(net::format_ipv4(0) == "0.0.0.0");
  CHECK(net::parse_ipv4("255.255.255.255", &addr));
  CHECK(addr == net::kIpv4Broadcast);
  CHECK_FALSE(net::parse_ipv4("", &addr));
  CHECK_FALSE(net::parse_ipv4("1.2.3", &addr));
  CHECK_FALSE(net::parse_ipv4("1.2.3.4.5", &addr));
  CHECK_FALSE(net::parse_ipv4("256.1.1.1", &addr));
  CHECK_FALSE(net::parse_ipv4("1.2.3.4 ", &addr));
  CHECK_FALSE(net::parse_ipv4("eth0", &addr));
  CHECK_FALSE(net::parse_ipv4("1111.1.1.1", &addr));

  CHECK(net::ipv4_is_private(0x0A000001));        // 10.0.0.1
  CHECK(net::ipv4_is_private(0xAC100001));        // 172.16.0.1
  CHECK(net::ipv4_is_private(0xAC1FFFFF));        // 172.31.255.255
  CHECK_FALSE(net::ipv4_is_private(0xAC200001));  // 172.32.0.1
  CHECK(net::ipv4_is_private(0xC0A80101));        // 192.168.1.1
  CHECK(net::ipv4_is_private(0xA9FE0001));        // 169.254.0.1
  CHECK_FALSE(net::ipv4_is_private(0x08080808));  // 8.8.8.8
  CHECK(net::ipv4_is_loopback(0x7F000001));
  CHECK(net::ipv4_is_multicast(0xE0000001));
}

TEST_CASE("Local-network-only peers", "[net]") {
  const uint32_t own = 0xC0A80114;        // 192.168.1.20
  const uint32_t mask = 0xFFFFFF00;       // /24
  CHECK(net::ipv4_is_lan_peer(0xC0A80115, own, mask));
  CHECK(net::ipv4_is_lan_peer(0x0A000002, own, mask));  // another private range
  CHECK(net::ipv4_is_lan_peer(0x7F000001, own, mask));
  CHECK(net::ipv4_is_lan_peer(0xFFFFFFFF, own, mask));
  CHECK(net::ipv4_is_lan_peer(0xC0A801FF, own, mask));  // directed broadcast
  CHECK_FALSE(net::ipv4_is_lan_peer(0x08080808, own, mask));
  CHECK_FALSE(net::ipv4_is_lan_peer(0, own, mask));
  CHECK_FALSE(net::ipv4_is_lan_peer(0xE0000001, own, mask));
  // A LAN on a public range (or a VPN interface) counts through its subnet.
  CHECK(net::ipv4_is_lan_peer(0x64400005, 0x64400002, 0xFFFFFF00));
  CHECK_FALSE(net::ipv4_is_lan_peer(0x64400005, 0, 0));

  CHECK(net::ipv4_directed_broadcast(own, mask) == 0xC0A801FF);
  CHECK(net::ipv4_directed_broadcast(0x0A010203, 0xFF000000) == 0x0AFFFFFF);
  CHECK(net::ipv4_directed_broadcast(own, 0xFFFFFFFF) == net::kIpv4Broadcast);
  CHECK(net::ipv4_directed_broadcast(own, 0) == net::kIpv4Broadcast);
}

TEST_CASE("Guest socket options and ioctls work on a host socket", "[net]") {
  [[maybe_unused]] WinsockInit winsock;
  LoopbackUdp sock;
  REQUIRE(sock.handle != net::kInvalidSocket);
  auto host = static_cast<HostSocket>(sock.handle);

  // setsockopt(s, SOL_SOCKET 0xFFFF, SO_BROADCAST 0x20, TRUE)
  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x20, 1) == 0);
  int value = 0;
  HostLength value_len = sizeof(value);
  getsockopt(host, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&value), &value_len);
  CHECK(value != 0);

  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x1002, 65536) == 0);  // SO_RCVBUF
  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x0080, 0x00010005) == 0);  // SO_LINGER
  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x1006, 250) == 0);         // SO_RCVTIMEO
  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x5801, 1) == 0);           // ignored
  CHECK(net::socket_set_option(sock.handle, 0xFFFF, 0x1234, 1) == 10042);       // WSAENOPROTOOPT

  // ioctlsocket(s, FIONBIO, 1): an empty socket then says "would block".
  uint32_t arg = 1;
  CHECK(net::socket_ioctl(sock.handle, 0x8004667E, &arg) == 0);
  char buffer[64];
  sockaddr_in from = {};
  HostLength from_len = sizeof(from);
  int got = static_cast<int>(recvfrom(host, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len));
  CHECK(got < 0);
  CHECK(net::socket_last_error() == 10035);

  // A datagram to itself; FIONREAD then counts its bytes.
  sockaddr_in self = {};
  self.sin_family = AF_INET;
  self.sin_port = htons(sock.port);
  self.sin_addr.s_addr = htonl(net::kIpv4Loopback);
  const char payload[] = "system link";
  REQUIRE(sendto(host, payload, sizeof(payload), 0, reinterpret_cast<sockaddr*>(&self),
                 sizeof(self)) == static_cast<int>(sizeof(payload)));
  uint32_t waiting = 0;
  for (int i = 0; i < 100 && !waiting; ++i) {
    CHECK(net::socket_ioctl(sock.handle, 0x4004667F, &waiting) == 0);
  }
  CHECK(waiting >= sizeof(payload));

  // Unknown ioctls are refused.
  uint32_t unused = 0;
  CHECK(net::socket_ioctl(sock.handle, 0x12345678, &unused) == 10022);
}

TEST_CASE("Random bytes differ between calls", "[net]") {
  std::array<uint8_t, 32> a{};
  std::array<uint8_t, 32> b{};
  CHECK(net::random_bytes(a.data(), a.size()));
  CHECK(net::random_bytes(b.data(), b.size()));
  CHECK(a != b);
}

TEST_CASE("Interface listing and default route don't fail", "[net]") {
  auto interfaces = net::list_ipv4_interfaces();
  for (auto& entry : interfaces) {
    CHECK(entry.address != 0);
  }
  uint32_t route = net::default_route_ipv4();
  CHECK((route == 0 || !net::ipv4_is_multicast(route)));
}
