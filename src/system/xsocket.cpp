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

#include <algorithm>
#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex::system {

namespace {

sockaddr_in ToHost(const rex::net::SockAddrV4& addr) {
  sockaddr_in host = {};
  host.sin_family = AF_INET;
  host.sin_port = htons(addr.port);
  host.sin_addr.s_addr = htonl(addr.addr);
  return host;
}

rex::net::SockAddrV4 FromHost(const sockaddr_in& host) {
  rex::net::SockAddrV4 addr;
  addr.addr = ntohl(host.sin_addr.s_addr);
  addr.port = ntohs(host.sin_port);
  return addr;
}

#if REX_PLATFORM_WIN32
using HostSocket = SOCKET;
using HostLength = int;
constexpr int kNoSignal = 0;
#else
using HostSocket = int;
using HostLength = socklen_t;
#if defined(MSG_NOSIGNAL)
// A send on a broken connection must fail, not raise SIGPIPE.
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;  // macOS: SO_NOSIGPIPE is set when it's opened
#endif
#endif

// Largest buffer passed to the host in one call (the largest IPv4 datagram).
constexpr uint32_t kMaxTransfer = 0x10000;

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

uint32_t XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  // Only IPv4 TCP and UDP (and VDP, sent as UDP) reach the host.
  if (af != X_AF_INET) {
    return rex::net::wsa::kEafnosupport;
  }
  int host_proto = 0;
  if (type == X_SOCK_DGRAM) {
    if (proto != 0 && proto != X_IPPROTO_UDP && proto != X_IPPROTO_VDP) {
      return rex::net::wsa::kEprotonosupport;
    }
    host_proto = IPPROTO_UDP;
  } else if (type == X_SOCK_STREAM) {
    if (proto != 0 && proto != X_IPPROTO_TCP) {
      return rex::net::wsa::kEprotonosupport;
    }
    host_proto = IPPROTO_TCP;
  } else {
    return rex::net::wsa::kEsocktnosupport;
  }

  auto handle = rex::net::socket_open(AF_INET, type == X_SOCK_DGRAM ? SOCK_DGRAM : SOCK_STREAM,
                                      host_proto);
  if (handle == rex::net::kInvalidSocket) {
    return rex::net::socket_last_error();
  }
  native_handle_ = static_cast<uint64_t>(handle);
  return 0;
}

X_STATUS XSocket::Close() {
  // Closed once: a second close could hit a descriptor reused since.
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }
  int ret = rex::net::socket_close(static_cast<rex::net::SocketHandle>(native_handle_));
  native_handle_ = uint64_t(-1);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

uint32_t XSocket::SetOption(uint32_t level, uint32_t optname, const uint8_t* optval,
                            uint32_t optlen) {
  auto option = rex::net::translate_socket_option(level, optname);
  if (option.kind == rex::net::OptionKind::kIgnored) {
    if (level == rex::net::wsa::kSolSocket && (optname == rex::net::wsa::kSoXboxInsecure ||
                                               optname == rex::net::wsa::kSoXboxNoEncrypt)) {
      // Disable socket encryption
      secure_ = false;
    }
    return 0;
  }
  if (option.kind == rex::net::OptionKind::kUnsupported) {
    return rex::net::wsa::kEnoprotoopt;
  }
  // Every supported option is 4 bytes on Winsock (an int, BOOL, DWORD or
  // two u_shorts), stored big-endian by the guest.
  if (!optval || optlen < 4) {
    return rex::net::wsa::kEfault;
  }
  uint32_t value = rex::memory::load_and_swap<uint32_t>(optval);
  uint32_t error = rex::net::socket_set_option(
      static_cast<rex::net::SocketHandle>(native_handle_), level, optname, value);
  if (error) {
    return error;
  }

  if (level == rex::net::wsa::kSolSocket && optname == rex::net::wsa::kSoBroadcast) {
    broadcast_socket_ = value != 0;
  }
  return 0;
}

uint32_t XSocket::IOControl(uint32_t cmd, uint32_t* value) {
  return rex::net::socket_ioctl(static_cast<rex::net::SocketHandle>(native_handle_), cmd, value);
}

uint32_t XSocket::Connect(const rex::net::SockAddrV4& to) {
  sockaddr_in host = ToHost(to);
  int ret = connect(static_cast<HostSocket>(native_handle_), reinterpret_cast<sockaddr*>(&host),
                    sizeof(host));
  return ret == 0 ? 0 : rex::net::socket_last_error();
}

uint32_t XSocket::Bind(const rex::net::SockAddrV4& name) {
  sockaddr_in host = ToHost(name);
  int ret = bind(static_cast<HostSocket>(native_handle_), reinterpret_cast<sockaddr*>(&host),
                 sizeof(host));
  if (ret != 0) {
    return rex::net::socket_last_error();
  }

  bound_ = true;
  bound_port_ = name.port;
  // Port 0 means "any": remember the one the host picked.
  sockaddr_in local = {};
  HostLength local_len = sizeof(local);
  if (getsockname(static_cast<HostSocket>(native_handle_), reinterpret_cast<sockaddr*>(&local),
                  &local_len) == 0) {
    bound_port_ = ntohs(local.sin_port);
  }
  return 0;
}

uint32_t XSocket::Listen(int backlog) {
  int ret = listen(static_cast<HostSocket>(native_handle_), backlog);
  return ret == 0 ? 0 : rex::net::socket_last_error();
}

object_ref<XSocket> XSocket::Accept(rex::net::SockAddrV4* from, uint32_t* error) {
  sockaddr_in host = {};
  HostLength host_len = sizeof(host);
  auto ret = accept(static_cast<HostSocket>(native_handle_), reinterpret_cast<sockaddr*>(&host),
                    &host_len);
#if REX_PLATFORM_WIN32
  bool failed = ret == INVALID_SOCKET;
#else
  bool failed = ret < 0;
#endif
  if (failed) {
    *error = rex::net::socket_last_error();
    return nullptr;
  }
  *error = 0;
  if (from) {
    *from = FromHost(host);
  }

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, static_cast<uint64_t>(ret)));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how, uint32_t* error) {
  int ret = shutdown(static_cast<HostSocket>(native_handle_), how);
  *error = ret == 0 ? 0 : rex::net::socket_last_error();
  return ret == 0 ? 0 : -1;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags, uint32_t* error) {
  int host_flags = 0;
  if (!rex::net::translate_message_flags(flags, &host_flags)) {
    *error = rex::net::wsa::kEopnotsupp;
    return -1;
  }
  int ret = static_cast<int>(recv(static_cast<HostSocket>(native_handle_),
                                  reinterpret_cast<char*>(buf),
                                  std::min(buf_len, kMaxTransfer), host_flags));
  *error = ret < 0 ? rex::net::socket_last_error() : 0;
  return ret < 0 ? -1 : ret;
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, rex::net::SockAddrV4* from,
                      uint32_t* error, uint32_t* full_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  int host_flags = 0;
  if (!rex::net::translate_message_flags(flags, &host_flags)) {
    *error = rex::net::wsa::kEopnotsupp;
    return -1;
  }
  buf_len = std::min(buf_len, kMaxTransfer);
#if defined(MSG_TRUNC) && !REX_PLATFORM_WIN32
  // Linux: MSG_TRUNC returns the datagram's real length, so a datagram too
  // big for the buffer can fail with WSAEMSGSIZE like it does on Windows.
  if (type_ == X_SOCK_DGRAM) {
    host_flags |= MSG_TRUNC;
  }
#endif
  sockaddr_in host = {};
  HostLength host_len = sizeof(host);
  int ret = static_cast<int>(recvfrom(static_cast<HostSocket>(native_handle_),
                                      reinterpret_cast<char*>(buf), buf_len, host_flags,
                                      reinterpret_cast<sockaddr*>(&host), &host_len));
  if (ret < 0) {
    *error = rex::net::socket_last_error();
    if (*error == rex::net::wsa::kEmsgsize && from) {
      // Windows: the buffer holds the start of the datagram.
      *from = FromHost(host);
    }
    if (full_len) {
      *full_len = 0;
    }
    return -1;
  }
  if (from) {
    *from = FromHost(host);
  }
  if (full_len) {
    *full_len = static_cast<uint32_t>(ret);
  }
  if (static_cast<uint32_t>(ret) > buf_len) {
    *error = rex::net::wsa::kEmsgsize;
    return -1;
  }
  *error = 0;
  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags, uint32_t* error) {
  int host_flags = 0;
  if (!rex::net::translate_message_flags(flags, &host_flags)) {
    *error = rex::net::wsa::kEopnotsupp;
    return -1;
  }
  int ret = static_cast<int>(send(static_cast<HostSocket>(native_handle_),
                                  reinterpret_cast<const char*>(buf),
                                  std::min(buf_len, kMaxTransfer), host_flags | kNoSignal));
  *error = ret < 0 ? rex::net::socket_last_error() : 0;
  return ret < 0 ? -1 : ret;
}

int XSocket::SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    const rex::net::SockAddrV4* to, uint32_t* error) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  int host_flags = 0;
  if (!rex::net::translate_message_flags(flags, &host_flags)) {
    *error = rex::net::wsa::kEopnotsupp;
    return -1;
  }
  if (buf_len > kMaxTransfer) {
    *error = rex::net::wsa::kEmsgsize;
    return -1;
  }
  sockaddr_in host = {};
  if (to) {
    host = ToHost(*to);
  }
  int ret = static_cast<int>(sendto(static_cast<HostSocket>(native_handle_),
                                    reinterpret_cast<const char*>(buf), buf_len,
                                    host_flags | kNoSignal,
                                    to ? reinterpret_cast<sockaddr*>(&host) : nullptr,
                                    to ? static_cast<HostLength>(sizeof(host)) : 0));
  *error = ret < 0 ? rex::net::socket_last_error() : 0;
  return ret < 0 ? -1 : ret;
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
