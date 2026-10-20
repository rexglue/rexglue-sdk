#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <queue>

#include <rex/math.h>
#include <rex/net/socket.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::countof(sa_data));
    return *this;
  }

  uint16_t address_family;
  char sa_data[14];
};

struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;

  // Always big-endian!
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  uint16_t sin_family;
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }
  Type type() const { return type_; }
  Protocol protocol() const { return proto_; }
  // VDP sockets carry a 2-byte game data length before the data. The guest
  // writes and reads it itself; on PCs VDP is sent as plain UDP.
  bool is_vdp() const { return proto_ == X_IPPROTO_VDP; }
  bool is_broadcast() const { return broadcast_socket_; }

  // The methods below return 0 or a Winsock error code (WSAE*), which the
  // caller passes to the guest through WSAGetLastError. Addresses are IPv4
  // in host byte order.
  uint32_t Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  // `optval` points at the guest's value (big-endian), `optlen` bytes long.
  uint32_t SetOption(uint32_t level, uint32_t optname, const uint8_t* optval, uint32_t optlen);
  // `value` is in and out, in host byte order.
  uint32_t IOControl(uint32_t cmd, uint32_t* value);

  uint32_t Connect(const rex::net::SockAddrV4& to);
  uint32_t Bind(const rex::net::SockAddrV4& name);
  uint32_t Listen(int backlog);
  object_ref<XSocket> Accept(rex::net::SockAddrV4* from, uint32_t* error);
  int Shutdown(int how, uint32_t* error);

  // These return the byte count, or -1 with `*error` set.
  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags, uint32_t* error);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags, uint32_t* error);
  // A datagram longer than `buf_len` fills the buffer and fails with
  // WSAEMSGSIZE, as on Windows and the console; `*full_len` (if given)
  // receives its real length.
  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, rex::net::SockAddrV4* from,
               uint32_t* error, uint32_t* full_len = nullptr);
  int SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags,
             const rex::net::SockAddrV4* to, uint32_t* error);

  struct packet {
    // These values are in network byte order.
    rex::be<uint16_t> src_port;
    rex::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  uint64_t native_handle_ = -1;

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;

  std::unique_ptr<rex::thread::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;
};

}  // namespace rex::system
