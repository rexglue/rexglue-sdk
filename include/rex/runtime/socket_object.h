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

#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/math.h>
#include <rex/move_only_function.h>
#include <rex/net/socket.h>
#include <rex/platform/thread.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

class IObjectTable;
class PinnedSlot;
struct PendingReceive;

/** Guest layout of a generic socket address. */
struct XSOCKADDR {
  be_u16 address_family; /**< Address family. */
  char sa_data[14];      /**< Address bytes. */
};

/** Host byte order copy of `XSOCKADDR`. */
struct N_XSOCKADDR {
  /** Builds an uninitialized address. */
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  /**
   * Copies a guest address, converting the family to host byte order.
   *
   * @param other The guest address.
   * @return This address.
   */
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::CountOf(sa_data));
    return *this;
  }

  u16 address_family; /**< Address family. */
  char sa_data[14];   /**< Address bytes. */
};

/** Guest layout of an IPv4 socket address. */
struct XSOCKADDR_IN {
  be_u16 sin_family; /**< Address family. */

  be_u16 sin_port; /**< Port. */
  be_u32 sin_addr; /**< IPv4 address. */
  /** Prefixed because Android defines sin_zero as __pad. */
  char x_sin_zero[8]; /**< Zero padding. */
};

/** Native sockaddr_in. */
struct N_XSOCKADDR_IN {
  /** Builds an uninitialized address. */
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  /**
   * Copies a guest address, converting the family to host byte order and zeroing the padding.
   *
   * @param other The guest address.
   * @return This address.
   */
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  u16 sin_family;  /**< Address family. */
  be_u16 sin_port; /**< Port. */
  be_u32 sin_addr; /**< IPv4 address. */
  /** Prefixed because Android defines sin_zero as __pad. */
  char x_sin_zero[8]; /**< Zero padding. */
};

/** The address families a socket object supports. */
enum SocketAddressFamily : u8 {
  kXAFInet = 2, /**< IPv4 address family. */
};

/** The socket types a socket object supports. */
enum SocketType : u8 {
  kXSockStream = 1, /**< Reliable byte stream socket. */
  kXSockDgram = 2,  /**< Datagram socket. */
};

/** The protocols a socket object supports. */
enum SocketProtocol : u8 {
  kXIPProtoTCP = 6,  /**< TCP over IPv4. */
  kXIPProtoUDP = 17, /**< UDP over IPv4. */

  /**
   * LIVE Voice and Data Protocol, framed as
   * [cbGameData][GameData(encrypted)][VoiceData(unencrypted)].
   */
  kXIPProtoVDP = 254,
};

/** The outcome of 1 receive attempt or of an overlapped receive. */
struct ReceiveResult {
  /** True when CancelReceive ended the receive before a datagram arrived. */
  bool cancelled = false;
  /** True when the attempt failed only because no datagram was waiting. */
  bool would_block = false;
  int received = -1;   /**< The byte count, -1 on failure. */
  u32 error = 0;       /**< The socket error of a failed receive, 0 otherwise. */
  N_XSOCKADDR_IN from; /**< The sender. */
  u32 from_len = 0;    /**< The size of the native sender address. */
};

/** Writes the end of an overlapped receive into guest memory and signals it. */
using ReceiveCompletion = rex::MoveOnlyFunction<void(const ReceiveResult&)>;

/** The host state of a socket object. */
struct SocketObject {
  /**
   * @param af_in The address family.
   * @param type_in The socket type.
   * @param proto_in The protocol.
   */
  REX_RUNTIME_API SocketObject(SocketAddressFamily af_in, SocketType type_in,
                               SocketProtocol proto_in);

  /** Closes the native socket. */
  REX_RUNTIME_API ~SocketObject();

  SocketAddressFamily af; /**< Address family the socket was created with. */
  SocketType type;        /**< Socket type the socket was created with. */
  SocketProtocol proto;   /**< Protocol the socket was created with. */
  /** Cleared when the title sets the option that disables the secure transport. */
  bool secure = true;
  bool bound = false; /**< Set by a successful bind. No code reads it. */
  /** Port in network byte order passed to the last successful bind. No code reads it. */
  u16 bound_port = 0;
  /** Set when the title enabled the broadcast option. No code reads it. */
  bool broadcast_socket = false;
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
   * Creates the native socket for the address family, type and protocol of the object. The native
   * socket never blocks, and a call the title made blocking waits for readiness instead.
   *
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS Initialize();

  /**
   * Marks the socket closed and wakes every call waiting on it. The native socket stays open until
   * the object is destroyed.
   *
   * @return True when this call marked the socket closed, false when it was already closed.
   */
  REX_RUNTIME_API bool Cancel();

  /**
   * Sets a socket option from a value in guest layout, big endian, translating the level, the
   * name and the value to the host.
   *
   * @param level The title option level.
   * @param optname The title option name.
   * @param optval_ptr The option value.
   * @param optlen The size of the option value.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_SUPPORTED for an option the platform does not define,
   *         X_STATUS_BUFFER_TOO_SMALL, X_STATUS_INVALID_PARAMETER, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS SetOption(u32 level, u32 optname, void* optval_ptr, u32 optlen);

  /**
   * Reads a socket option and writes it in guest layout, big endian.
   *
   * @param level The title option level.
   * @param optname The title option name.
   * @param optval_ptr Receives the option value.
   * @param optlen The size of the buffer, then the size of the value.
   * @return X_STATUS_SUCCESS, X_STATUS_NOT_SUPPORTED for an option the platform does not define,
   *         X_STATUS_BUFFER_TOO_SMALL, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS GetOption(u32 level, u32 optname, void* optval_ptr, u32* optlen) const;

  /**
   * Runs an IO control command.
   *
   * @param cmd The command.
   * @param arg_ptr The command argument.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS IOControl(u32 cmd, u8* arg_ptr);

  /**
   * Connects to an address.
   *
   * @param name The address.
   * @param name_len The size of the address.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS Connect(N_XSOCKADDR* name, u32 name_len) const;

  /**
   * Binds to an address.
   *
   * @param name The address.
   * @param name_len The size of the address.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS Bind(N_XSOCKADDR_IN* name, u32 name_len);

  /**
   * Starts listening for connections.
   *
   * @param backlog The length of the pending connection queue.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_RUNTIME_API X_STATUS Listen(int backlog) const;

  /**
   * Accepts a connection and creates a socket object for it.
   *
   * @param runtime The runtime the call runs in.
   * @param name Receives the peer address.
   * @param name_len Receives the size of the peer address.
   * @return The handle of the new socket with a count of 1, or kXInvalidHandleValue.
   */
  REX_RUNTIME_API X_HANDLE Accept(Runtime& runtime, N_XSOCKADDR* name, int* name_len);

  /**
   * Shuts a connection down.
   *
   * @param how Which directions to shut down.
   * @return The native result.
   */
  REX_RUNTIME_API int Shutdown(int how) const;

  /**
   * Receives a datagram.
   *
   * @param buf The buffer.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @param from Receives the sender, may be null.
   * @param from_len Receives the size of the sender address, may be null.
   * @return The native result.
   */
  REX_RUNTIME_API int RecvFrom(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from,
                               u32* from_len) const;

  /**
   * Receives data.
   *
   * @param buf The buffer.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @return The native result.
   */
  REX_RUNTIME_API int Recv(u8* buf, u32 buf_len, u32 flags) const;

  /**
   * Receives a datagram once, without waiting.
   *
   * @param buf The buffer.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @return The outcome of the attempt.
   */
  REX_RUNTIME_API ReceiveResult TryRecvFrom(u8* buf, u32 buf_len, u32 flags) const;

  /**
   * Receives a datagram on a new thread once 1 arrives, then calls complete on that thread. The
   * receive holds pin and 1 reference on the first handle until a joiner releases them.
   *
   * @param table The table that owns the socket.
   * @param pin A pin on this socket.
   * @param buf The buffer, which must stay mapped until the receive is joined.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @param complete Called once with the outcome.
   * @return False when a receive is pending or the receive cannot start.
   */
  REX_RUNTIME_API bool StartReceive(IObjectTable& table, PinnedSlot pin, u8* buf, u32 buf_len,
                                    u32 flags, ReceiveCompletion complete);

  /**
   * Stops the pending receive and joins its thread. A receive that had not ended gets its
   * completion called as cancelled before this returns.
   */
  REX_RUNTIME_API void CancelReceive();

  /**
   * Sends a datagram.
   *
   * @param buf The buffer.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @param to The destination, may be null.
   * @param to_len The size of the destination address.
   * @return The native result.
   */
  REX_RUNTIME_API int SendTo(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to, u32 to_len) const;

  /**
   * Sends data.
   *
   * @param buf The buffer.
   * @param buf_len The size of the buffer.
   * @param flags The native flags.
   * @return The native result.
   */
  REX_RUNTIME_API int Send(const u8* buf, u32 buf_len, u32 flags) const;
};

}  // namespace rex::runtime
