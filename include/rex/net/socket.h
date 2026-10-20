/**
 * @file        net/socket.h
 * @brief       Platform-agnostic socket operations
 *
 * The guest uses the Xbox 360's Winsock numbers for socket options, ioctls,
 * message flags and error codes. Windows shares them; Linux and macOS don't.
 * These helpers translate them so a socket behaves the same on every host.
 * Addresses here are IPv4 in host byte order (192.168.1.2 is 0xC0A80102).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rex::net {

using SocketHandle = int64_t;
constexpr SocketHandle kInvalidSocket = -1;

// Winsock (and Xbox 360) numbers, as the guest passes them.
namespace wsa {
constexpr uint32_t kSolSocket = 0xFFFF;
constexpr uint32_t kIpprotoTcp = 6;

constexpr uint32_t kSoDebug = 0x0001;
constexpr uint32_t kSoReuseAddr = 0x0004;
constexpr uint32_t kSoKeepAlive = 0x0008;
constexpr uint32_t kSoDontRoute = 0x0010;
constexpr uint32_t kSoBroadcast = 0x0020;
constexpr uint32_t kSoLinger = 0x0080;
constexpr uint32_t kSoOobInline = 0x0100;
constexpr uint32_t kSoDontLinger = ~kSoLinger;
constexpr uint32_t kSoExclusiveAddrUse = ~kSoReuseAddr;
constexpr uint32_t kSoSndBuf = 0x1001;
constexpr uint32_t kSoRcvBuf = 0x1002;
constexpr uint32_t kSoSndTimeo = 0x1005;
constexpr uint32_t kSoRcvTimeo = 0x1006;
// Xbox 360 only: turn the console's socket encryption off. Meaningless
// between PCs, so they are accepted and ignored.
constexpr uint32_t kSoXboxInsecure = 0x5801;
constexpr uint32_t kSoXboxNoEncrypt = 0x5802;
constexpr uint32_t kTcpNoDelay = 0x0001;

constexpr uint32_t kFionbio = 0x8004667E;
constexpr uint32_t kFionread = 0x4004667F;

constexpr uint32_t kMsgOob = 0x1;
constexpr uint32_t kMsgPeek = 0x2;
constexpr uint32_t kMsgDontRoute = 0x4;

constexpr uint32_t kEintr = 10004;
constexpr uint32_t kEbadf = 10009;
constexpr uint32_t kEacces = 10013;
constexpr uint32_t kEfault = 10014;
constexpr uint32_t kEinval = 10022;
constexpr uint32_t kEmfile = 10024;
constexpr uint32_t kEwouldblock = 10035;
constexpr uint32_t kEinprogress = 10036;
constexpr uint32_t kEalready = 10037;
constexpr uint32_t kEnotsock = 10038;
constexpr uint32_t kEdestaddrreq = 10039;
constexpr uint32_t kEmsgsize = 10040;
constexpr uint32_t kEprototype = 10041;
constexpr uint32_t kEnoprotoopt = 10042;
constexpr uint32_t kEprotonosupport = 10043;
constexpr uint32_t kEsocktnosupport = 10044;
constexpr uint32_t kEopnotsupp = 10045;
constexpr uint32_t kEpfnosupport = 10046;
constexpr uint32_t kEafnosupport = 10047;
constexpr uint32_t kEaddrinuse = 10048;
constexpr uint32_t kEaddrnotavail = 10049;
constexpr uint32_t kEnetdown = 10050;
constexpr uint32_t kEnetunreach = 10051;
constexpr uint32_t kEnetreset = 10052;
constexpr uint32_t kEconnaborted = 10053;
constexpr uint32_t kEconnreset = 10054;
constexpr uint32_t kEnobufs = 10055;
constexpr uint32_t kEisconn = 10056;
constexpr uint32_t kEnotconn = 10057;
constexpr uint32_t kEshutdown = 10058;
constexpr uint32_t kEtimedout = 10060;
constexpr uint32_t kEconnrefused = 10061;
constexpr uint32_t kEhostdown = 10064;
constexpr uint32_t kEhostunreach = 10065;
}  // namespace wsa

// An IPv4 address and port, both in host byte order.
struct SockAddrV4 {
  uint32_t addr = 0;
  uint16_t port = 0;
};

// How a guest socket option is applied on the host.
enum class OptionKind : uint8_t {
  kUnsupported,  // unknown here: refused with WSAENOPROTOOPT
  kIgnored,      // accepted and ignored
  kInt,          // a 32-bit integer or BOOL
  kLinger,       // struct linger: l_onoff and l_linger, 16 bits each on Winsock
  kDontLinger,   // Winsock's SO_DONTLINGER: SO_LINGER with l_onoff inverted
  kTimeoutMs,    // milliseconds (a DWORD on Winsock, a timeval on POSIX)
};

struct HostOption {
  OptionKind kind = OptionKind::kUnsupported;
  int level = 0;
  int name = 0;
};

// Maps a guest (Winsock) option level and name to the host's numbers. Only
// the options listed here are passed to the host; anything else is refused.
HostOption translate_socket_option(uint32_t level, uint32_t optname);

// Opens a socket that child processes don't inherit (the game restarts
// itself to switch between the campaign and multiplayer programs).
// Returns kInvalidSocket on failure (see socket_last_error()).
SocketHandle socket_open(int af, int type, int protocol);
int socket_close(SocketHandle handle);

// Sets a guest socket option. `value` is the option's 32-bit value as the
// guest stores it (for SO_LINGER: l_onoff in the high 16 bits, l_linger in
// the low 16). Returns 0 or a Winsock error code.
uint32_t socket_set_option(SocketHandle handle, uint32_t level, uint32_t optname, uint32_t value);

// Winsock ioctlsocket: FIONBIO (`*value` nonzero = non-blocking) and
// FIONREAD (`*value` receives the bytes waiting). Returns 0 or a Winsock
// error code; other commands give WSAEINVAL.
uint32_t socket_ioctl(SocketHandle handle, uint32_t cmd, uint32_t* value);

// Winsock message flags (MSG_OOB, MSG_PEEK, MSG_DONTROUTE) to the host's.
// Returns false if `flags` has any other bit set.
bool translate_message_flags(uint32_t flags, int* host_flags);

// The Winsock error code of the calling thread's last failed socket call:
// WSAGetLastError() on Windows, errno translated elsewhere.
uint32_t socket_last_error();

#if !defined(_WIN32)
// An errno value as the Winsock error code Windows would give (POSIX only).
uint32_t wsa_error_from_errno(int err);
#endif

// Fills `out` from the system's cryptographic random source.
bool random_bytes(void* out, size_t length);

// A network interface's IPv4 address, in host byte order.
struct Ipv4Interface {
  std::string name;  // the interface name (POSIX; empty on Windows)
  uint32_t address = 0;
  uint32_t netmask = 0;
  bool up = false;
  bool loopback = false;
  bool broadcast = false;
};

// The host's IPv4 interfaces (empty if they can't be listed).
std::vector<Ipv4Interface> list_ipv4_interfaces();

// The local address the host would send from toward the default route, or
// 0 if there is none. Nothing is sent: it asks the routing table only.
uint32_t default_route_ipv4();

constexpr uint32_t kIpv4Loopback = 0x7F000001;
constexpr uint32_t kIpv4Broadcast = 0xFFFFFFFF;

constexpr bool ipv4_is_loopback(uint32_t addr) {
  return (addr >> 24) == 127;
}

// RFC 1918 private ranges and link-local 169.254.0.0/16.
constexpr bool ipv4_is_private(uint32_t addr) {
  return (addr >> 24) == 10 || (addr >> 20) == 0xAC1 || (addr >> 16) == 0xC0A8 ||
         (addr >> 16) == 0xA9FE;
}

constexpr bool ipv4_is_multicast(uint32_t addr) {
  return (addr >> 28) == 0xE;
}

// Whether a local-network-only game may exchange packets with `addr`: this
// PC (loopback), the limited broadcast, private and link-local addresses,
// and the subnet of `own` (the chosen interface's address and netmask).
constexpr bool ipv4_is_lan_peer(uint32_t addr, uint32_t own, uint32_t netmask) {
  if (!addr || ipv4_is_multicast(addr)) {
    return false;
  }
  if (ipv4_is_loopback(addr) || addr == kIpv4Broadcast || ipv4_is_private(addr)) {
    return true;
  }
  return own && netmask && (addr & netmask) == (own & netmask);
}

// The broadcast address of `own`'s subnet (the limited broadcast when the
// netmask leaves no host bits).
constexpr uint32_t ipv4_directed_broadcast(uint32_t own, uint32_t netmask) {
  if (!netmask || netmask == 0xFFFFFFFF || netmask == 0xFFFFFFFE) {
    return kIpv4Broadcast;
  }
  return own | ~netmask;
}

// Parses a dotted-quad IPv4 address ("192.168.1.2"); nothing else.
inline bool parse_ipv4(std::string_view text, uint32_t* out) {
  uint32_t result = 0;
  size_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    if (part) {
      if (pos >= text.size() || text[pos] != '.') {
        return false;
      }
      ++pos;
    }
    uint32_t value = 0;
    size_t digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && digits < 3) {
      value = value * 10 + uint32_t(text[pos] - '0');
      ++pos;
      ++digits;
    }
    if (!digits || value > 255) {
      return false;
    }
    result = (result << 8) | value;
  }
  if (pos != text.size()) {
    return false;
  }
  *out = result;
  return true;
}

inline std::string format_ipv4(uint32_t addr) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
                (addr >> 8) & 0xFF, addr & 0xFF);
  return text;
}

}  // namespace rex::net
