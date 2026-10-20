#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// RtlGenRandom (advapi32): the system's cryptographic random source.
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);

namespace rex::net {

HostOption translate_socket_option(uint32_t level, uint32_t optname) {
  // Windows uses the guest's own Winsock numbers; only the options listed
  // here reach the host, as on the other systems.
  if (level == wsa::kSolSocket) {
    switch (optname) {
      case wsa::kSoXboxInsecure:
      case wsa::kSoXboxNoEncrypt:
        return {OptionKind::kIgnored, 0, 0};
      case wsa::kSoDebug:
      case wsa::kSoReuseAddr:
      case wsa::kSoKeepAlive:
      case wsa::kSoDontRoute:
      case wsa::kSoBroadcast:
      case wsa::kSoOobInline:
      case wsa::kSoSndBuf:
      case wsa::kSoRcvBuf:
      case wsa::kSoExclusiveAddrUse:
        return {OptionKind::kInt, SOL_SOCKET, static_cast<int>(optname)};
      case wsa::kSoLinger:
        return {OptionKind::kLinger, SOL_SOCKET, SO_LINGER};
      case wsa::kSoDontLinger:
        return {OptionKind::kDontLinger, SOL_SOCKET, SO_LINGER};
      case wsa::kSoSndTimeo:
        return {OptionKind::kTimeoutMs, SOL_SOCKET, SO_SNDTIMEO};
      case wsa::kSoRcvTimeo:
        return {OptionKind::kTimeoutMs, SOL_SOCKET, SO_RCVTIMEO};
      default:
        break;
    }
  } else if (level == wsa::kIpprotoTcp && optname == wsa::kTcpNoDelay) {
    return {OptionKind::kInt, IPPROTO_TCP, TCP_NODELAY};
  }
  return {};
}

SocketHandle socket_open(int af, int type, int protocol) {
  SOCKET s = socket(af, type, protocol);
  if (s == INVALID_SOCKET) {
    return kInvalidSocket;
  }
  SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
  if (type == SOCK_DGRAM) {
    // Windows reports an ICMP "port unreachable" as WSAECONNRESET on a later
    // receive; Linux and the console don't for unconnected UDP. Turn it off
    // so every host behaves the same.
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
  }
  return static_cast<SocketHandle>(s);
}

int socket_close(SocketHandle handle) {
  return closesocket(static_cast<SOCKET>(handle));
}

uint32_t socket_set_option(SocketHandle handle, uint32_t level, uint32_t optname, uint32_t value) {
  HostOption option = translate_socket_option(level, optname);
  SOCKET s = static_cast<SOCKET>(handle);
  int ret = 0;
  switch (option.kind) {
    case OptionKind::kUnsupported:
      return wsa::kEnoprotoopt;
    case OptionKind::kIgnored:
      return 0;
    case OptionKind::kInt:
    case OptionKind::kTimeoutMs: {
      DWORD host_value = value;
      ret = setsockopt(s, option.level, option.name, reinterpret_cast<const char*>(&host_value),
                       sizeof(host_value));
      break;
    }
    case OptionKind::kLinger:
    case OptionKind::kDontLinger: {
      linger host_linger = {};
      host_linger.l_onoff = static_cast<u_short>((value >> 16) != 0);
      host_linger.l_linger = static_cast<u_short>(value & 0xFFFF);
      if (option.kind == OptionKind::kDontLinger) {
        host_linger.l_onoff = static_cast<u_short>(value == 0);
        host_linger.l_linger = 0;
      }
      ret = setsockopt(s, option.level, option.name, reinterpret_cast<const char*>(&host_linger),
                       sizeof(host_linger));
      break;
    }
  }
  return ret == 0 ? 0 : socket_last_error();
}

uint32_t socket_ioctl(SocketHandle handle, uint32_t cmd, uint32_t* value) {
  if (cmd != wsa::kFionbio && cmd != wsa::kFionread) {
    return wsa::kEinval;
  }
  u_long host_value = *value;
  if (ioctlsocket(static_cast<SOCKET>(handle), static_cast<long>(cmd), &host_value) != 0) {
    return socket_last_error();
  }
  *value = static_cast<uint32_t>(host_value);
  return 0;
}

bool translate_message_flags(uint32_t flags, int* host_flags) {
  if (flags & ~(wsa::kMsgOob | wsa::kMsgPeek | wsa::kMsgDontRoute)) {
    return false;
  }
  *host_flags = static_cast<int>(flags);  // same numbers on Windows
  return true;
}

uint32_t socket_last_error() {
  return static_cast<uint32_t>(WSAGetLastError());
}

bool random_bytes(void* out, size_t length) {
  auto* bytes = static_cast<uint8_t*>(out);
  while (length) {
    ULONG n = length > 0x10000 ? 0x10000 : static_cast<ULONG>(length);
    if (!SystemFunction036(bytes, n)) {
      return false;
    }
    bytes += n;
    length -= n;
  }
  return true;
}

namespace {
// Winsock must be started for the queries below; the guest may not have
// called WSAStartup yet. Each scope starts and stops it once.
struct WinsockScope {
  bool ok;
  WinsockScope() {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockScope() {
    if (ok) {
      WSACleanup();
    }
  }
};
}  // namespace

std::vector<Ipv4Interface> list_ipv4_interfaces() {
  std::vector<Ipv4Interface> result;
  WinsockScope winsock;
  if (!winsock.ok) {
    return result;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) {
    return result;
  }
  // SIO_GET_INTERFACE_LIST needs only ws2_32 (Wine has it too).
  INTERFACE_INFO list[64];
  DWORD bytes = 0;
  if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list), &bytes, nullptr,
               nullptr) == 0) {
    size_t count = bytes / sizeof(INTERFACE_INFO);
    if (count > 64) {
      count = 64;
    }
    for (size_t i = 0; i < count; ++i) {
      const INTERFACE_INFO& entry = list[i];
      if (entry.iiAddress.Address.sa_family != AF_INET) {
        continue;
      }
      Ipv4Interface info;
      info.address = ntohl(entry.iiAddress.AddressIn.sin_addr.s_addr);
      info.netmask = ntohl(entry.iiNetmask.AddressIn.sin_addr.s_addr);
      info.up = (entry.iiFlags & IFF_UP) != 0;
      info.loopback = (entry.iiFlags & IFF_LOOPBACK) != 0;
      info.broadcast = (entry.iiFlags & IFF_BROADCAST) != 0;
      result.push_back(std::move(info));
    }
  }
  closesocket(s);
  return result;
}

uint32_t default_route_ipv4() {
  WinsockScope winsock;
  if (!winsock.ok) {
    return 0;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) {
    return 0;
  }
  // Connecting a UDP socket only picks a route and a source address; no
  // packet leaves the machine. 192.0.2.1 is a documentation-only address.
  sockaddr_in target = {};
  target.sin_family = AF_INET;
  target.sin_port = htons(9);
  target.sin_addr.s_addr = htonl(0xC0000201);
  uint32_t result = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in local = {};
    int local_len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
      result = ntohl(local.sin_addr.s_addr);
    }
  }
  closesocket(s);
  return result;
}

}  // namespace rex::net
