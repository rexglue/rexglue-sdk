/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <cstring>
#include <limits>

#include <rex/net/socket.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xsocket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

#include "runtime/thread_wait.h"

namespace rex::runtime {

constexpr u32 kMaxSocketLength = static_cast<u32>(std::numeric_limits<i32>::max());

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

sockaddr_in ToHost(const N_XSOCKADDR_IN& name) {
  sockaddr_in host{};
  host.sin_family = name.sin_family;
  host.sin_port = htons(name.sin_port);
  host.sin_addr.s_addr = htonl(name.sin_addr);
  return host;
}

sockaddr_in ToHost(const N_XSOCKADDR& name) {
  sockaddr_in host{};
  host.sin_family = name.address_family;
  std::memcpy(&host.sin_port, name.sa_data, sizeof(host.sin_port));
  std::memcpy(&host.sin_addr, name.sa_data + sizeof(host.sin_port), sizeof(host.sin_addr));
  return host;
}

void FromHost(const sockaddr_in& host, N_XSOCKADDR& name) {
  name.address_family = host.sin_family;
  std::memset(name.sa_data, 0, sizeof(name.sa_data));
  std::memcpy(name.sa_data, &host.sin_port, sizeof(host.sin_port));
  std::memcpy(name.sa_data + sizeof(host.sin_port), &host.sin_addr, sizeof(host.sin_addr));
}

}  // namespace

SocketObject::SocketObject(SocketAddressFamily af_in, SocketType type_in, SocketProtocol proto_in)
    : af(af_in), type(type_in), proto(proto_in) {}

SocketObject::~SocketObject() {
  SocketClose(*this);
}

X_STATUS SocketInitialize(SocketObject& sock) {
  SocketProtocol native_proto = sock.proto;
  if (native_proto == X_IPPROTO_VDP) {
    native_proto = X_IPPROTO_UDP;
  }

  sock.native_handle = socket(sock.af, sock.type, native_proto);
  if (sock.native_handle == static_cast<u64>(-1)) {
    return X_STATUS_UNSUCCESSFUL;
  }
#if defined(SO_NOSIGPIPE)
  const int enable = 1;
  if (setsockopt(sock.native_handle, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
#endif

  return Prepare(sock);
}

X_STATUS SocketClose(SocketObject& sock) {
  if (sock.native_handle == static_cast<u64>(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::platform::socket_close(sock.native_handle);
  sock.native_handle = static_cast<u64>(-1);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketSetOption(SocketObject& sock, u32 level, u32 optname, void* optval_ptr, u32 optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    sock.secure = false;
    return X_STATUS_SUCCESS;
  }

  if (optlen > kMaxSocketLength) {
    return X_STATUS_INVALID_PARAMETER;
  }
  int ret = setsockopt(sock.native_handle, level, optname, static_cast<char*>(optval_ptr),
                       static_cast<int>(optlen));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (level == 0xFFFF && optname == 0x0020) {
    sock.broadcast_socket = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketIOControl(SocketObject& sock, u32 cmd, u8* arg_ptr) {
  if (cmd == kFIONBIO && arg_ptr) {
    u32 value = 0;
    std::memcpy(&value, arg_ptr, sizeof(value));
    sock.nonblocking = value != 0;
    return X_STATUS_SUCCESS;
  }

  int ret = rex::platform::socket_ioctl(sock.native_handle, cmd, arg_ptr);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketConnect(SocketObject& sock, N_XSOCKADDR* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = connect(sock.native_handle, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketBind(SocketObject& sock, N_XSOCKADDR_IN* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = bind(sock.native_handle, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  sock.bound = true;
  sock.bound_port = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS SocketListen(SocketObject& sock, int backlog) {
  int ret = listen(sock.native_handle, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_HANDLE SocketAccept(Runtime& runtime, SocketObject& sock, N_XSOCKADDR* name, int* name_len) {
  sockaddr_in n_sockaddr{};
  socklen_t n_name_len = sizeof(n_sockaddr);
  uintptr_t ret = accept(sock.native_handle, reinterpret_cast<sockaddr*>(&n_sockaddr), &n_name_len);
  if (ret == static_cast<uintptr_t>(-1)) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return X_INVALID_HANDLE_VALUE;
  }

  FromHost(n_sockaddr, *name);
  *name_len = sizeof(XSOCKADDR_IN);

  IObjectTable& table = runtime.object_table();
  Result<X_HANDLE> added =
      table.Emplace<SocketObject>(AddOptions{}, sock.af, sock.type, sock.proto);
  if (!added) {
    rex::platform::socket_close(ret);
    return X_INVALID_HANDLE_VALUE;
  }
  const X_HANDLE handle = *added;
  auto accepted = table.Get<SocketObject>(handle);
  accepted->native_handle = ret;
  accepted->nonblocking = sock.nonblocking;
  if (XFAILED(Prepare(*accepted))) {
    CloseOwned(table, handle);
    return X_INVALID_HANDLE_VALUE;
  }

  return handle;
}

int SocketShutdown(SocketObject& sock, int how) {
  return shutdown(sock.native_handle, how);
}

int SocketRecv(SocketObject& sock, u8* buf, u32 buf_len, u32 flags) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  int ret;
  do {
    ret = recv(sock.native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
               static_cast<int>(flags));
  } while (ret == -1 && WaitToRetry(sock, SocketReady::kRead));
  return ret;
}

int SocketRecvFrom(SocketObject& sock, u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from,
                   u32* from_len) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  sockaddr_in nfrom{};
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret;
  do {
    nfromlen = sizeof(sockaddr_in);
    ret = recvfrom(sock.native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
                   static_cast<int>(flags), reinterpret_cast<sockaddr*>(&nfrom), &nfromlen);
  } while (ret == -1 && WaitToRetry(sock, SocketReady::kRead));
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);
    from->sin_port = ntohs(nfrom.sin_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int SocketSend(SocketObject& sock, const u8* buf, u32 buf_len, u32 flags) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  u32 sent = 0;
  while (true) {
    const int ret = send(sock.native_handle, reinterpret_cast<const char*>(buf + sent),
                         static_cast<int>(buf_len - sent), static_cast<int>(flags) | kSendFlags);
    if (ret == -1) {
      if (WaitToRetry(sock, SocketReady::kWrite)) {
        continue;
      }
      return sent > 0 ? static_cast<int>(sent) : -1;
    }
    sent += static_cast<u32>(ret);
    if (sent >= buf_len || sock.nonblocking || sock.type != X_SOCK_STREAM) {
      return static_cast<int>(sent);
    }
  }
}

int SocketSendTo(SocketObject& sock, u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to,
                 u32 to_len) {
  if ((to && to_len < sizeof(XSOCKADDR_IN)) || buf_len > kMaxSocketLength) {
    return -1;
  }
  const sockaddr_in nto = to ? ToHost(*to) : sockaddr_in{};

  int ret;
  do {
    ret = sendto(sock.native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
                 static_cast<int>(flags) | kSendFlags,
                 to ? reinterpret_cast<const sockaddr*>(&nto) : nullptr,
                 to ? static_cast<socklen_t>(sizeof(nto)) : 0);
  } while (ret == -1 && WaitToRetry(sock, SocketReady::kWrite));
  return ret;
}

}  // namespace rex::runtime
