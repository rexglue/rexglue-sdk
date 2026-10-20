#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if REX_PLATFORM_LINUX
#include <sys/random.h>
#else
#include <stdlib.h>  // arc4random_buf
#endif

#include <random>

namespace rex::net {

HostOption translate_socket_option(uint32_t level, uint32_t optname) {
  if (level == wsa::kSolSocket) {
    switch (optname) {
      case wsa::kSoXboxInsecure:
      case wsa::kSoXboxNoEncrypt:
      // Binding without SO_REUSEADDR is already exclusive on POSIX.
      case wsa::kSoExclusiveAddrUse:
        return {OptionKind::kIgnored, 0, 0};
      case wsa::kSoDebug:
        return {OptionKind::kInt, SOL_SOCKET, SO_DEBUG};
      case wsa::kSoReuseAddr:
        return {OptionKind::kInt, SOL_SOCKET, SO_REUSEADDR};
      case wsa::kSoKeepAlive:
        return {OptionKind::kInt, SOL_SOCKET, SO_KEEPALIVE};
      case wsa::kSoDontRoute:
        return {OptionKind::kInt, SOL_SOCKET, SO_DONTROUTE};
      case wsa::kSoBroadcast:
        return {OptionKind::kInt, SOL_SOCKET, SO_BROADCAST};
      case wsa::kSoLinger:
        return {OptionKind::kLinger, SOL_SOCKET, SO_LINGER};
      case wsa::kSoDontLinger:
        return {OptionKind::kDontLinger, SOL_SOCKET, SO_LINGER};
      case wsa::kSoOobInline:
        return {OptionKind::kInt, SOL_SOCKET, SO_OOBINLINE};
      case wsa::kSoSndBuf:
        return {OptionKind::kInt, SOL_SOCKET, SO_SNDBUF};
      case wsa::kSoRcvBuf:
        return {OptionKind::kInt, SOL_SOCKET, SO_RCVBUF};
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
#if defined(SOCK_CLOEXEC)
  int fd = socket(af, type | SOCK_CLOEXEC, protocol);
#else
  int fd = socket(af, type, protocol);
  if (fd >= 0) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
#endif
#if REX_PLATFORM_MAC
  if (fd >= 0) {
    // macOS has no MSG_NOSIGNAL: don't let a broken connection raise SIGPIPE.
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  return fd < 0 ? kInvalidSocket : SocketHandle(fd);
}

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

uint32_t socket_set_option(SocketHandle handle, uint32_t level, uint32_t optname, uint32_t value) {
  HostOption option = translate_socket_option(level, optname);
  int fd = static_cast<int>(handle);
  int ret = 0;
  switch (option.kind) {
    case OptionKind::kUnsupported:
      return wsa::kEnoprotoopt;
    case OptionKind::kIgnored:
      return 0;
    case OptionKind::kInt: {
      int host_value = static_cast<int>(value);
      ret = setsockopt(fd, option.level, option.name, &host_value, sizeof(host_value));
      break;
    }
    case OptionKind::kLinger:
    case OptionKind::kDontLinger: {
      linger host_linger = {};
      host_linger.l_onoff = (value >> 16) != 0;
      host_linger.l_linger = static_cast<int>(value & 0xFFFF);
      if (option.kind == OptionKind::kDontLinger) {
        // SO_DONTLINGER takes a BOOL: true turns lingering off.
        host_linger.l_onoff = value == 0;
        host_linger.l_linger = 0;
      }
      ret = setsockopt(fd, option.level, option.name, &host_linger, sizeof(host_linger));
      break;
    }
    case OptionKind::kTimeoutMs: {
      timeval host_timeout = {};
      host_timeout.tv_sec = static_cast<time_t>(value / 1000);
      host_timeout.tv_usec = static_cast<suseconds_t>((value % 1000) * 1000);
      ret = setsockopt(fd, option.level, option.name, &host_timeout, sizeof(host_timeout));
      break;
    }
  }
  return ret == 0 ? 0 : socket_last_error();
}

uint32_t socket_ioctl(SocketHandle handle, uint32_t cmd, uint32_t* value) {
  int fd = static_cast<int>(handle);
  if (cmd == wsa::kFionbio) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      return socket_last_error();
    }
    flags = *value ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0 ? 0 : socket_last_error();
  }
  if (cmd == wsa::kFionread) {
    int available = 0;
    if (ioctl(fd, FIONREAD, &available) != 0) {
      return socket_last_error();
    }
    *value = available > 0 ? static_cast<uint32_t>(available) : 0;
    return 0;
  }
  return wsa::kEinval;
}

bool translate_message_flags(uint32_t flags, int* host_flags) {
  if (flags & ~(wsa::kMsgOob | wsa::kMsgPeek | wsa::kMsgDontRoute)) {
    return false;
  }
  int result = 0;
  if (flags & wsa::kMsgOob) result |= MSG_OOB;
  if (flags & wsa::kMsgPeek) result |= MSG_PEEK;
  if (flags & wsa::kMsgDontRoute) result |= MSG_DONTROUTE;
  *host_flags = result;
  return true;
}

uint32_t wsa_error_from_errno(int err) {
  switch (err) {
    case 0:
      return 0;
    case EINTR:
      return wsa::kEintr;
    case EBADF:
      return wsa::kEbadf;
    case EACCES:
    case EPERM:
      return wsa::kEacces;
    case EFAULT:
      return wsa::kEfault;
    case EINVAL:
      return wsa::kEinval;
    case EMFILE:
    case ENFILE:
      return wsa::kEmfile;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EAGAIN:
      return wsa::kEwouldblock;
    case EINPROGRESS:
      return wsa::kEinprogress;
    case EALREADY:
      return wsa::kEalready;
    case ENOTSOCK:
      return wsa::kEnotsock;
    case EDESTADDRREQ:
      return wsa::kEdestaddrreq;
    case EMSGSIZE:
      return wsa::kEmsgsize;
    case EPROTOTYPE:
      return wsa::kEprototype;
    case ENOPROTOOPT:
      return wsa::kEnoprotoopt;
    case EPROTONOSUPPORT:
      return wsa::kEprotonosupport;
    case ESOCKTNOSUPPORT:
      return wsa::kEsocktnosupport;
#if EOPNOTSUPP != ENOTSUP
    case ENOTSUP:
#endif
    case EOPNOTSUPP:
      return wsa::kEopnotsupp;
    case EPFNOSUPPORT:
      return wsa::kEpfnosupport;
    case EAFNOSUPPORT:
      return wsa::kEafnosupport;
    case EADDRINUSE:
      return wsa::kEaddrinuse;
    case EADDRNOTAVAIL:
      return wsa::kEaddrnotavail;
    case ENETDOWN:
      return wsa::kEnetdown;
    case ENETUNREACH:
      return wsa::kEnetunreach;
    case ENETRESET:
      return wsa::kEnetreset;
    case ECONNABORTED:
      return wsa::kEconnaborted;
    case ECONNRESET:
    case EPIPE:
      return wsa::kEconnreset;
    case ENOBUFS:
    case ENOMEM:
      return wsa::kEnobufs;
    case EISCONN:
      return wsa::kEisconn;
    case ENOTCONN:
      return wsa::kEnotconn;
    case ESHUTDOWN:
      return wsa::kEshutdown;
    case ETIMEDOUT:
      return wsa::kEtimedout;
    case ECONNREFUSED:
      return wsa::kEconnrefused;
    case EHOSTDOWN:
      return wsa::kEhostdown;
    case EHOSTUNREACH:
      return wsa::kEhostunreach;
    default:
      // Winsock has no exact match; WSAEINVAL is what most games handle.
      return wsa::kEinval;
  }
}

uint32_t socket_last_error() {
  return wsa_error_from_errno(errno);
}

bool random_bytes(void* out, size_t length) {
  auto* bytes = static_cast<uint8_t*>(out);
#if REX_PLATFORM_LINUX
  while (length) {
    ssize_t got = getrandom(bytes, length, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    bytes += got;
    length -= static_cast<size_t>(got);
  }
  if (!length) {
    return true;
  }
  // Very old kernels lack getrandom: the C++ library's source then.
  try {
    std::random_device device;
    while (length) {
      uint32_t word = device();
      size_t n = length < sizeof(word) ? length : sizeof(word);
      std::memcpy(bytes, &word, n);
      bytes += n;
      length -= n;
    }
    return true;
  } catch (...) {
    return false;
  }
#else
  arc4random_buf(bytes, length);
  return true;
#endif
}

std::vector<Ipv4Interface> list_ipv4_interfaces() {
  std::vector<Ipv4Interface> result;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    return result;
  }
  for (ifaddrs* entry = list; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    Ipv4Interface info;
    info.name = entry->ifa_name ? entry->ifa_name : "";
    info.address = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
    if (entry->ifa_netmask && entry->ifa_netmask->sa_family == AF_INET) {
      info.netmask =
          ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr);
    }
    info.up = (entry->ifa_flags & IFF_UP) && (entry->ifa_flags & IFF_RUNNING);
    info.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
    info.broadcast = (entry->ifa_flags & IFF_BROADCAST) != 0;
    result.push_back(std::move(info));
  }
  freeifaddrs(list);
  return result;
}

uint32_t default_route_ipv4() {
  int fd = static_cast<int>(socket_open(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    return 0;
  }
  // Connecting a UDP socket only picks a route and a source address; no
  // packet leaves the machine. 192.0.2.1 is a documentation-only address.
  sockaddr_in target = {};
  target.sin_family = AF_INET;
  target.sin_port = htons(9);
  target.sin_addr.s_addr = htonl(0xC0000201);
  uint32_t result = 0;
  if (connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in local = {};
    socklen_t local_len = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
      result = ntohl(local.sin_addr.s_addr);
    }
  }
  close(fd);
  return result;
}

}  // namespace rex::net
