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
#include <memory>

#include <rex/macros.h>
#include <rex/math.h>
#include <rex/platform/thread.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

struct XSOCKADDR {
  be_u16 address_family;
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
  be_u16 sin_family;

  // Always big-endian!
  be_u16 sin_port;
  be_u32 sin_addr;
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
  be_u16 sin_port;
  be_u32 sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

/** The address families a socket object supports. */
enum SocketAddressFamily {
  X_AF_INET = 2,
};

/** The socket types a socket object supports. */
enum SocketType {
  X_SOCK_STREAM = 1,
  X_SOCK_DGRAM = 2,
};

/** The protocols a socket object supports. */
enum SocketProtocol {
  X_IPPROTO_TCP = 6,
  X_IPPROTO_UDP = 17,

  // LIVE Voice and Data Protocol
  // https://blog.csdn.net/baozi3026/article/details/4277227
  // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
  X_IPPROTO_VDP = 254,
};

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

  u64 native_handle = static_cast<u64>(-1);
  SocketAddressFamily af;
  SocketType type;
  SocketProtocol proto;
  bool secure = true;
  bool bound = false;
  u16 bound_port = 0;
  bool broadcast_socket = false;
  bool nonblocking = false;
  std::unique_ptr<rex::platform::Event> event;
};

/**
 * Creates the native socket for the address family, type and protocol of the object. The native
 * socket never blocks, and a call the title made blocking waits for readiness instead.
 *
 * @param sock The socket.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketInitialize(SocketObject& sock);

/**
 * Closes the native socket.
 *
 * @param sock The socket.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketClose(SocketObject& sock);

/**
 * Sets a socket option.
 *
 * @param sock The socket.
 * @param level The option level.
 * @param optname The option name.
 * @param optval_ptr The option value.
 * @param optlen The size of the option value.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketSetOption(SocketObject& sock, u32 level, u32 optname,
                                         void* optval_ptr, u32 optlen);

/**
 * Runs an IO control command.
 *
 * @param sock The socket.
 * @param cmd The command.
 * @param arg_ptr The command argument.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketIOControl(SocketObject& sock, u32 cmd, u8* arg_ptr);

/**
 * Connects to an address.
 *
 * @param sock The socket.
 * @param name The address.
 * @param name_len The size of the address.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketConnect(SocketObject& sock, N_XSOCKADDR* name, int name_len);

/**
 * Binds to an address.
 *
 * @param sock The socket.
 * @param name The address.
 * @param name_len The size of the address.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketBind(SocketObject& sock, N_XSOCKADDR_IN* name, int name_len);

/**
 * Starts listening for connections.
 *
 * @param sock The socket.
 * @param backlog The length of the pending connection queue.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS SocketListen(SocketObject& sock, int backlog);

/**
 * Accepts a connection and creates a socket object for it.
 *
 * @param runtime The runtime the call runs in.
 * @param sock The listening socket.
 * @param name Receives the peer address.
 * @param name_len Receives the size of the peer address.
 * @return The handle of the new socket with a count of 1, or X_INVALID_HANDLE_VALUE.
 */
REX_RUNTIME_API X_HANDLE SocketAccept(Runtime& runtime, SocketObject& sock, N_XSOCKADDR* name,
                                      int* name_len);

/**
 * Shuts a connection down.
 *
 * @param sock The socket.
 * @param how Which directions to shut down.
 * @return The native result.
 */
REX_RUNTIME_API int SocketShutdown(SocketObject& sock, int how);

/**
 * Receives data.
 *
 * @param sock The socket.
 * @param buf The buffer.
 * @param buf_len The size of the buffer.
 * @param flags The native flags.
 * @return The native result.
 */
REX_RUNTIME_API int SocketRecv(SocketObject& sock, u8* buf, u32 buf_len, u32 flags);

/**
 * Sends data.
 *
 * @param sock The socket.
 * @param buf The buffer.
 * @param buf_len The size of the buffer.
 * @param flags The native flags.
 * @return The native result.
 */
REX_RUNTIME_API int SocketSend(SocketObject& sock, const u8* buf, u32 buf_len, u32 flags);

/**
 * Receives a datagram.
 *
 * @param sock The socket.
 * @param buf The buffer.
 * @param buf_len The size of the buffer.
 * @param flags The native flags.
 * @param from Receives the sender, may be null.
 * @param from_len Receives the size of the sender address, may be null.
 * @return The native result.
 */
REX_RUNTIME_API int SocketRecvFrom(SocketObject& sock, u8* buf, u32 buf_len, u32 flags,
                                   N_XSOCKADDR_IN* from, u32* from_len);

/**
 * Sends a datagram.
 *
 * @param sock The socket.
 * @param buf The buffer.
 * @param buf_len The size of the buffer.
 * @param flags The native flags.
 * @param to The destination, may be null.
 * @param to_len The size of the destination address.
 * @return The native result.
 */
REX_RUNTIME_API int SocketSendTo(SocketObject& sock, u8* buf, u32 buf_len, u32 flags,
                                 N_XSOCKADDR_IN* to, u32 to_len);

}  // namespace rex::runtime
