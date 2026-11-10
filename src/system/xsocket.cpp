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

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
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
#include <sys/time.h>
#endif

namespace rex::system {

namespace {

// SOL_SOCKET and its options as the guest numbers them (WinSock's values).
constexpr uint32_t kXSolSocket = 0xFFFF;
constexpr uint32_t kXSoReuseAddr = 0x0004;
constexpr uint32_t kXSoBroadcast = 0x0020;
constexpr uint32_t kXSoLinger = 0x0080;
constexpr uint32_t kXSoSndBuf = 0x1001;
constexpr uint32_t kXSoRcvBuf = 0x1002;
constexpr uint32_t kXSoSndTimeo = 0x1005;
constexpr uint32_t kXSoRcvTimeo = 0x1006;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
uint16_t LoadBE16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = -1;
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == kXSolSocket && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  // The guest uses WinSock's numbering (SOL_SOCKET is 0xFFFF, 1 on Linux) and
  // writes its values big-endian, so passing the buffer straight to the host
  // either fails (unknown level/option on POSIX) or sets a byte-swapped value
  // (a 128 KiB SO_RCVBUF becomes 512 bytes). Convert the SOL_SOCKET options
  // titles use; anything else goes through unchanged, as before.
  int native_level = static_cast<int>(level);
  int native_optname = static_cast<int>(optname);
  const char* value = static_cast<const char*>(optval_ptr);
  socklen_t value_len = static_cast<socklen_t>(optlen);
  const auto* guest = static_cast<const uint8_t*>(optval_ptr);
  int int_value = 0;
  linger linger_value{};
#if !REX_PLATFORM_WIN32
  timeval timeout_value{};
#endif
  bool converted = false;
  if (level == kXSolSocket && guest) {
    switch (optname) {
      case kXSoReuseAddr:
      case kXSoBroadcast:
        // BOOL. Titles pass 4 bytes or a single byte; any non-zero byte is true.
        for (uint32_t i = 0; i < optlen; ++i) {
          int_value |= guest[i] != 0;
        }
        native_optname = optname == kXSoReuseAddr ? SO_REUSEADDR : SO_BROADCAST;
        converted = optlen > 0;
        break;
      case kXSoSndBuf:
      case kXSoRcvBuf:
        int_value = static_cast<int>(LoadBE32(guest));
        native_optname = optname == kXSoSndBuf ? SO_SNDBUF : SO_RCVBUF;
        converted = optlen == 4;
        break;
      case kXSoLinger:
        // WinSock's struct linger is two u_shorts.
        linger_value.l_onoff = LoadBE16(guest);
        linger_value.l_linger = LoadBE16(guest + 2);
        native_optname = SO_LINGER;
        converted = optlen == 4;
        if (converted) {
          value = reinterpret_cast<const char*>(&linger_value);
          value_len = sizeof(linger_value);
        }
        break;
      case kXSoSndTimeo:
      case kXSoRcvTimeo: {
        // WinSock: a DWORD in milliseconds. POSIX: a struct timeval.
        const uint32_t ms = LoadBE32(guest);
        native_optname = optname == kXSoSndTimeo ? SO_SNDTIMEO : SO_RCVTIMEO;
        converted = optlen == 4;
#if !REX_PLATFORM_WIN32
        if (converted) {
          timeout_value.tv_sec = ms / 1000;
          timeout_value.tv_usec = (ms % 1000) * 1000;
          value = reinterpret_cast<const char*>(&timeout_value);
          value_len = sizeof(timeout_value);
        }
#else
        int_value = static_cast<int>(ms);
#endif
        break;
      }
      default:
        break;
    }
    if (converted) {
      native_level = SOL_SOCKET;
      if (value == static_cast<const char*>(optval_ptr)) {
        value = reinterpret_cast<const char*>(&int_value);
        value_len = sizeof(int_value);
      }
    } else {
      native_optname = static_cast<int>(optname);
    }
  }

  int ret = setsockopt(native_handle_, native_level, native_optname, value, value_len);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

#if !REX_PLATFORM_WIN32 && defined(SO_REUSEPORT)
  // WinSock's SO_REUSEADDR lets several sockets bind the same address and port
  // (P2P titles open one per peer); POSIX hosts need SO_REUSEPORT for that.
  if (converted && optname == kXSoReuseAddr && int_value) {
    int one = 1;
    setsockopt(native_handle_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  }
#endif

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
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

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = nfrom.sin_port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
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

  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
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
