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

#include <cstring>

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

namespace rex::runtime {

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

  return X_STATUS_SUCCESS;
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

  int ret = setsockopt(sock.native_handle, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (level == 0xFFFF && optname == 0x0020) {
    sock.broadcast_socket = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketIOControl(SocketObject& sock, u32 cmd, u8* arg_ptr) {
  int ret = rex::platform::socket_ioctl(sock.native_handle, cmd, arg_ptr);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketConnect(SocketObject& sock, N_XSOCKADDR* name, int name_len) {
  int ret = connect(sock.native_handle, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketBind(SocketObject& sock, N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(sock.native_handle, (sockaddr*)name, name_len);
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
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(sock.native_handle, &n_sockaddr, &n_name_len);
  if (ret == static_cast<uintptr_t>(-1)) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return X_INVALID_HANDLE_VALUE;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  IObjectTable& table = runtime.object_table();
  Result<X_HANDLE> added =
      table.Emplace<SocketObject>(AddOptions{}, sock.af, sock.type, sock.proto);
  if (!added) {
    rex::platform::socket_close(ret);
    return X_INVALID_HANDLE_VALUE;
  }
  const X_HANDLE handle = *added;
  table.Get<SocketObject>(handle)->native_handle = ret;

  return handle;
}

int SocketShutdown(SocketObject& sock, int how) {
  return shutdown(sock.native_handle, how);
}

int SocketRecv(SocketObject& sock, u8* buf, u32 buf_len, u32 flags) {
  return recv(sock.native_handle, reinterpret_cast<char*>(buf), buf_len, flags);
}

int SocketRecvFrom(SocketObject& sock, u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from,
                   u32* from_len) {
  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(sock.native_handle, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);
    from->sin_port = nfrom.sin_port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int SocketSend(SocketObject& sock, const u8* buf, u32 buf_len, u32 flags) {
  return send(sock.native_handle, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int SocketSendTo(SocketObject& sock, u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to,
                 u32 to_len) {
  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
  }

  return sendto(sock.native_handle, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

}  // namespace rex::runtime
