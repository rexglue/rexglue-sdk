/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/socket_object.h
 * @brief       Socket object, socket addresses and pending receives
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

#include <rex/platform.h>
#include <rex/math.h>
#include <rex/utility.h>
#include <rex/platform/socket.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>

namespace rex::xam {

inline constexpr u32 kXInvalidSocket = ~u32{0};
inline constexpr u32 kXSocketError = static_cast<u32>(-1);

/** Largest length in bytes a socket call accepts. */
inline constexpr u32 kMaxSocketLength = static_cast<u32>(std::numeric_limits<i32>::max());

struct PendingReceive;

/** Guest layout of a generic socket address. */
struct XSOCKADDR {
  be_u16 address_family;
  char sa_data[14];
};

/** Host byte order copy of `XSOCKADDR`. */
struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  /** Copies a guest address, converting the family to host byte order. */
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::CountOf(sa_data));
    return *this;
  }

  u16 address_family;
  char sa_data[14];
};

/** Guest layout of an IPv4 socket address. */
struct XSOCKADDR_IN {
  be_u16 sin_family;
  be_u16 sin_port;
  be_u32 sin_addr;
  /** Prefixed because Android defines sin_zero as __pad. */
  char x_sin_zero[8];
};

/** Native sockaddr_in. */
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  /** Copies a guest address, converting the family to host byte order and zeroing the padding. */
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  u16 sin_family;
  be_u16 sin_port;
  be_u32 sin_addr;
  /** Prefixed because Android defines sin_zero as __pad. */
  char x_sin_zero[8];
};

/** The address families a socket object supports. */
enum SocketAddressFamily : u8 {
  kXAFInet = 2,
};

/** The socket types a socket object supports. */
enum SocketType : u8 {
  kXSockStream = 1, /**< Reliable byte stream socket. */
  kXSockDgram = 2,
};

/** The protocols a socket object supports. */
enum SocketProtocol : u8 {
  kXIPProtoTCP = 6,
  kXIPProtoUDP = 17,

  /**
   * LIVE Voice and Data Protocol, framed as
   * [cbGameData][GameData(encrypted)][VoiceData(unencrypted)].
   */
  kXIPProtoVDP = 254,
};

/** The outcome of a receive attempt or of an overlapped receive. */
struct ReceiveResult {
  /** True when CancelReceive ended the receive before a datagram arrived. */
  bool canceled = false;
  /** True when the attempt failed only because no datagram was waiting. */
  bool would_block = false;
  int received = -1;   /**< The byte count, -1 on failure. */
  u32 error = 0;       /**< The socket error of a failed receive, 0 otherwise. */
  N_XSOCKADDR_IN from; /**< The sender. */
  u32 from_len = 0;    /**< The size of the native sender address. */
};

/** Called with the outcome of an overlapped receive. */
using ReceiveCompletion = rex::MoveOnlyFunction<void(const ReceiveResult&)>;

/** The host state of a socket object. */
struct SocketObject final : kernel::IXAMObject {
  static constexpr u32 kTag = 0x534F434B;

  SocketObject(SocketAddressFamily af_in, SocketType type_in, SocketProtocol proto_in);

  ~SocketObject() override;

  u32 GetTag() const override { return kTag; }

  SocketAddressFamily af;
  SocketType type;
  SocketProtocol proto;
  /** Cleared when the title sets the option that disables the secure transport. */
  bool secure = true;
  /** True after the title enabled non blocking mode, so calls return instead of waiting. */
  bool nonblocking = false;
  /** Event that Prepare binds to the native socket for readiness waits. */
  std::unique_ptr<rex::platform::Event> event;
  /** True once Cancel ran. Every later call fails, and the native socket stays open. */
  std::atomic<bool> closed = false;
  /** The native socket, set once by Initialize or Accept and closed with the object. */
  rex::platform::Socket socket;
  /** Guards pending_receive. The receive thread never takes it. */
  std::mutex receive_mutex;
  /** The overlapped receive that has not been joined, empty when there is none. */
  std::unique_ptr<PendingReceive> pending_receive;

  /**
   * Creates the native socket for the address family, type and protocol of the object.
   *
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  kernel::X_STATUS Initialize();

  /**
   * Marks the socket closed and wakes every call waiting on it. The native socket stays open until
   * the object is destroyed.
   *
   * @return True when this call marked the socket closed, false when it was already closed.
   */
  bool Cancel();

  /**
   * Sets a socket option from guest layout values.
   *
   * @param level The title option level.
   * @param optname The title option name.
   * @param optval_ptr The option value.
   * @param optlen The size of the option value.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_SUPPORTED for an option the platform does not define,
   *         X_STATUS_BUFFER_TOO_SMALL, X_STATUS_INVALID_PARAMETER, or X_STATUS_UNSUCCESSFUL.
   */
  kernel::X_STATUS SetOption(u32 level, u32 optname, void* optval_ptr, u32 optlen);

  /**
   * Reads a socket option into guest layout.
   *
   * @param level The title option level.
   * @param optname The title option name.
   * @param optval_ptr Receives the option value.
   * @param optlen The size of the buffer, then the size of the value.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_SUPPORTED for an option the platform does not define,
   *         X_STATUS_BUFFER_TOO_SMALL, or X_STATUS_UNSUCCESSFUL.
   */
  kernel::X_STATUS GetOption(u32 level, u32 optname, void* optval_ptr, u32* optlen) const;

  kernel::X_STATUS IOControl(u32 cmd, u8* arg_ptr);

  kernel::X_STATUS Connect(N_XSOCKADDR* name, u32 name_len) const;

  kernel::X_STATUS Bind(N_XSOCKADDR_IN* name, u32 name_len) const;

  /**
   * Returns the local address, which has the bound port and INADDR_ANY. It returns
   * X_STATUS_SUCCESS, X_STATUS_INVALID_PARAMETER_1 when the socket has no local port, or
   * X_STATUS_UNSUCCESSFUL.
   *
   * @param name Receives the local address.
   */
  kernel::X_STATUS GetName(N_XSOCKADDR* name) const;

  kernel::X_STATUS Listen(int backlog) const;

  /**
   * Accepts a connection and creates a socket object for it.
   *
   * @param name Receives the peer address.
   * @param name_len Receives the size of the peer address.
   * @return The handle of the new socket with a count of 1, or kXInvalidHandleValue.
   */
  kernel::X_HANDLE Accept(N_XSOCKADDR* name, int* name_len);

  int Shutdown(int how) const;

  int RecvFrom(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from, u32* from_len) const;

  int Recv(u8* buf, u32 buf_len, u32 flags) const;

  ReceiveResult TryRecvFrom(u8* buf, u32 buf_len, u32 flags) const;

  /**
   * Starts an overlapped receive that calls complete on its own thread. The receive holds pin and
   * 1 reference on the first handle until a joiner releases them.
   *
   * @param table The table that owns the socket.
   * @param pin A pin on this socket.
   * @param buf The buffer, which must stay mapped until the receive is joined.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @param complete Called once with the outcome.
   * @return False when a receive is pending or the receive cannot start.
   */
  bool StartReceive(kernel::IObjectTable& table, kernel::ObjectPin pin, u8* buf, u32 buf_len,
                    u32 flags, ReceiveCompletion complete);

  /**
   * Stops the pending receive and joins its thread. A receive that had not ended gets its
   * completion called as canceled before this returns.
   */
  void CancelReceive();

  int SendTo(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to, u32 to_len) const;

  int Send(const u8* buf, u32 buf_len, u32 flags) const;
};

/**
 * Cancels the pending receive of every socket in a table and shuts the sockets down, which wakes
 * every call waiting on one.
 *
 * @param table The table that owns the sockets.
 */
void ShutdownSockets(kernel::IObjectTable& table);

}  // namespace rex::xam
