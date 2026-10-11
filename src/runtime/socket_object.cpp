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
#include <rex/runtime/socket_object.h>

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
  Close();
}

X_STATUS SocketObject::Initialize() {
  SocketProtocol native_proto = proto;
  if (native_proto == X_IPPROTO_VDP) {
    native_proto = X_IPPROTO_UDP;
  }

  native_handle = socket(af, type, native_proto);
  if (native_handle == static_cast<u64>(-1)) {
    return X_STATUS_UNSUCCESSFUL;
  }
#if defined(SO_NOSIGPIPE)
  const int enable = 1;
  if (setsockopt(sock.native_handle, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
#endif

  return Prepare(*this);
}

X_STATUS SocketObject::Close() {
  if (native_handle == static_cast<u64>(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::platform::socket_close(static_cast<rex::platform::SocketHandle>(native_handle));
  native_handle = static_cast<u64>(-1);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::SetOption(u32 level, u32 optname, void* optval_ptr, u32 optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    secure = false;
    return X_STATUS_SUCCESS;
  }

  if (optlen > kMaxSocketLength) {
    return X_STATUS_INVALID_PARAMETER;
  }
  int ret = setsockopt(native_handle, static_cast<int>(level), static_cast<int>(optname),
                       static_cast<char*>(optval_ptr), static_cast<int>(optlen));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::IOControl(u32 cmd, u8* arg_ptr) {
  if (cmd == kFIONBIO && arg_ptr) {
    u32 value = 0;
    std::memcpy(&value, arg_ptr, sizeof(value));
    nonblocking = value != 0;
    return X_STATUS_SUCCESS;
  }

  int ret = rex::platform::socket_ioctl(static_cast<rex::platform::SocketHandle>(native_handle),
                                        cmd, arg_ptr);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Connect(N_XSOCKADDR* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = connect(native_handle, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0 && (nonblocking || !rex::platform::socket_would_block() || !WaitConnected(*this))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Bind(N_XSOCKADDR_IN* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = bind(native_handle, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound = true;
  bound_port = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Listen(int backlog) {
  int ret = listen(native_handle, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_HANDLE SocketObject::Accept(Runtime& runtime, N_XSOCKADDR* name, int* name_len) {
  sockaddr_in n_sockaddr{};
  socklen_t n_name_len = sizeof(n_sockaddr);
  uintptr_t ret;
  do {
    n_name_len = sizeof(n_sockaddr);
    ret = accept(native_handle, reinterpret_cast<sockaddr*>(&n_sockaddr), &n_name_len);
  } while (ret == static_cast<uintptr_t>(-1) && WaitToRetry(*this, SocketReady::kRead));
  if (ret == static_cast<uintptr_t>(-1)) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return X_INVALID_HANDLE_VALUE;
  }

  FromHost(n_sockaddr, *name);
  *name_len = sizeof(XSOCKADDR_IN);

  IObjectTable& table = runtime.object_table();
  Result<X_HANDLE> added = table.Emplace<SocketObject>(AddOptions{}, af, type, proto);
  if (!added) {
    rex::platform::socket_close(static_cast<rex::platform::SocketHandle>(ret));
    return X_INVALID_HANDLE_VALUE;
  }
  const X_HANDLE handle = *added;
  auto accepted = table.Get<SocketObject>(handle);
  accepted->native_handle = ret;
  accepted->nonblocking = nonblocking;
  if (XFAILED(Prepare(*accepted))) {
    CloseOwned(table, handle);
    return X_INVALID_HANDLE_VALUE;
  }

  return handle;
}

int SocketObject::Shutdown(int how) {
  return shutdown(native_handle, how);
}

int SocketObject::Recv(u8* buf, u32 buf_len, u32 flags) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  int ret;
  do {
    ret = recv(native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
               static_cast<int>(flags));
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kRead));
  return ret;
}

int SocketObject::RecvFrom(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from, u32* from_len) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  sockaddr_in nfrom{};
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret;
  do {
    nfromlen = sizeof(sockaddr_in);
    ret = recvfrom(native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
                   static_cast<int>(flags), reinterpret_cast<sockaddr*>(&nfrom), &nfromlen);
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kRead));
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

int SocketObject::Send(const u8* buf, u32 buf_len, u32 flags) {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  u32 sent = 0;
  while (true) {
    const int ret = send(native_handle, reinterpret_cast<const char*>(buf + sent),
                         static_cast<int>(buf_len - sent), static_cast<int>(flags) | kSendFlags);
    if (ret == -1) {
      if (WaitToRetry(*this, SocketReady::kWrite)) {
        continue;
      }
      return sent > 0 ? static_cast<int>(sent) : -1;
    }
    sent += static_cast<u32>(ret);
    if (sent >= buf_len || nonblocking || type != X_SOCK_STREAM) {
      return static_cast<int>(sent);
    }
  }
}

int SocketObject::SendTo(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to, u32 to_len) {
  if ((to && to_len < sizeof(XSOCKADDR_IN)) || buf_len > kMaxSocketLength) {
    return -1;
  }
  const sockaddr_in nto = to ? ToHost(*to) : sockaddr_in{};

  int ret;
  do {
    ret = sendto(native_handle, reinterpret_cast<char*>(buf), static_cast<int>(buf_len),
                 static_cast<int>(flags) | kSendFlags,
                 to ? reinterpret_cast<const sockaddr*>(&nto) : nullptr,
                 to ? static_cast<socklen_t>(sizeof(nto)) : 0);
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kWrite));
  return ret;
}

}  // namespace rex::runtime
