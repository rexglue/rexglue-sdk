/**
 * @file        net/socket.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <span>
#include <utility>

#include <rex/bit.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/types.h>

struct sockaddr;

namespace rex::platform {

/**
 * Native socket handle widened to 64 bits. It holds a SOCKET on Windows and a file descriptor on
 * POSIX.
 */
using SocketHandle = i64;

/** Handle value that names no socket. */
constexpr SocketHandle kInvalidSocket = -1;

/**
 * Closes a socket with closesocket on Windows or close on POSIX.
 *
 * @param handle The socket to close.
 * @return 0 on success, or the platform error return value.
 */
REX_API int SocketClose(SocketHandle handle);

/**
 * Runs an ioctl request on a socket with ioctlsocket on Windows or ioctl on POSIX.
 *
 * @param handle The socket to control.
 * @param cmd The platform ioctl request code.
 * @param arg The request argument buffer, read and written in place.
 * @return 0 on success, or the platform error return value.
 */
REX_API int SocketIoctl(SocketHandle handle, u32 cmd, u8* arg);

/** The readiness a socket wait asks for. */
enum class SocketReady : u8 {
  kNone = 0,        /**< No readiness requested. */
  kRead = 1 << 0,   /**< Data, a connection or a peer close to read. */
  kWrite = 1 << 1,  /**< Space to send, or a finished connect. */
  kExcept = 1 << 2, /**< Out of band data, or a failed connect. */
};

/** A socket a wait watches. */
struct SocketWait {
  SocketHandle handle = kInvalidSocket;   /**< The socket. */
  WaitHandle* event = nullptr;            /**< The readiness event passed to SocketPrepare. */
  SocketReady ready = SocketReady::kNone; /**< The readiness to wait for. */
};

/**
 * Makes a socket nonblocking and binds an event to its waits. On Windows the socket also sets the
 * event when it becomes ready.
 *
 * @param handle The socket.
 * @param event A manual reset event whose Set ends a WaitSockets on the socket.
 * @return 0 on success, or the platform error return value.
 */
REX_API int SocketPrepare(SocketHandle handle, WaitHandle* event);

/**
 * Reports whether the last socket call of the calling thread failed only because it would block.
 *
 * @return True when the error is a would block error or a connect in progress.
 */
REX_API bool SocketWouldBlock();

/**
 * Returns the last socket error of the calling thread.
 *
 * @return The error as a WSA code, WSAEINVAL when the host error has no WSA equivalent.
 */
REX_API u32 SocketError();

/**
 * Reports the readiness of each socket without waiting.
 *
 * @param sockets The sockets to check.
 * @param ready Receives, for each socket, the readiness it asked for that holds now.
 * @return The count of ready sockets, or -1 with SocketError set.
 */
REX_API int PollSockets(std::span<const SocketWait> sockets, std::span<SocketReady> ready);

/**
 * Waits until a socket may be ready.
 *
 * @param sockets The sockets to watch.
 * @param alertable True when a queued user callback ends the wait.
 * @param timeout The longest wait, milliseconds::max() for no limit.
 * @return kSuccess when a socket may be ready or its event is set, kUserCallback, kTimeout or
 *         kFailed.
 */
REX_API WaitResult WaitSockets(std::span<const SocketWait> sockets, bool alertable,
                               std::chrono::milliseconds timeout);

/**
 * Native socket owned by 1 value. The destructor closes it, and a move leaves the source empty.
 */
class Socket : public MoveOnly {
 public:
  /** Creates an empty socket. */
  Socket() = default;

  /**
   * Takes ownership of a native socket.
   *
   * @param handle The socket, or kInvalidSocket.
   */
  explicit Socket(SocketHandle handle) : handle_(handle) {}

  /** Closes the held socket. */
  ~Socket() { Reset(); }

  /**
   * Takes the native socket of another.
   *
   * @param other The socket to take, left empty.
   */
  Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, kInvalidSocket)) {}

  /**
   * Closes the held socket and takes another.
   *
   * @param other The socket to take, left empty.
   * @return This socket.
   */
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, kInvalidSocket);
    }
    return *this;
  }

  /**
   * Creates a native socket.
   *
   * @param family The address family.
   * @param type The socket type.
   * @param protocol The protocol.
   * @return The socket, empty with SocketError set on failure.
   */
  REX_API static Socket Create(i32 family, i32 type, i32 protocol);

  /**
   * Reports whether the socket holds a native socket.
   *
   * @return True when a native socket is held.
   */
  bool GetValid() const { return handle_ != kInvalidSocket; }

  /**
   * Returns the native socket.
   *
   * @return The handle, kInvalidSocket when empty.
   */
  SocketHandle GetHandle() const { return handle_; }

  /**
   * Connects to an address.
   *
   * @param name The host address.
   * @param name_len The size of the address.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 Connect(const sockaddr* name, i32 name_len) const;

  /**
   * Binds to an address.
   *
   * @param name The host address.
   * @param name_len The size of the address.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 Bind(const sockaddr* name, i32 name_len) const;

  /**
   * Starts listening for connections.
   *
   * @param backlog The length of the pending connection queue.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 Listen(i32 backlog) const;

  /**
   * Accepts a connection.
   *
   * @param name Receives the peer address.
   * @param name_len The size of the address buffer, then the size of the peer address.
   * @return The connected socket, empty with SocketError set on failure.
   */
  REX_API Socket Accept(sockaddr* name, i32* name_len) const;

  /**
   * Shuts a connection down.
   *
   * @param how Which directions to shut down.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 Shutdown(i32 how) const;

  /**
   * Receives data.
   *
   * @param buf The buffer.
   * @param len The size of the buffer.
   * @param flags The host flags.
   * @return The byte count, or -1 with SocketError set.
   */
  REX_API i32 Recv(void* buf, i32 len, i32 flags) const;

  /**
   * Receives a datagram.
   *
   * @param buf The buffer.
   * @param len The size of the buffer.
   * @param flags The host flags.
   * @param from Receives the sender address.
   * @param from_len The size of the address buffer, then the size of the sender address.
   * @return The byte count, or -1 with SocketError set.
   */
  REX_API i32 RecvFrom(void* buf, i32 len, i32 flags, sockaddr* from, i32* from_len) const;

  /**
   * Sends data. A send to a closed peer never raises a signal.
   *
   * @param buf The data.
   * @param len The size of the data.
   * @param flags The host flags.
   * @return The byte count, or -1 with SocketError set.
   */
  REX_API i32 Send(const void* buf, i32 len, i32 flags) const;

  /**
   * Sends a datagram. A send to a closed peer never raises a signal.
   *
   * @param buf The data.
   * @param len The size of the data.
   * @param flags The host flags.
   * @param to The destination, null for the connected peer.
   * @param to_len The size of the destination address.
   * @return The byte count, or -1 with SocketError set.
   */
  REX_API i32 SendTo(const void* buf, i32 len, i32 flags, const sockaddr* to, i32 to_len) const;

  /**
   * Sets a socket option.
   *
   * @param level The option level.
   * @param name The option name.
   * @param value The option value.
   * @param len The size of the value.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 SetOption(i32 level, i32 name, const void* value, i32 len) const;

  /**
   * Reads a socket option.
   *
   * @param level The option level.
   * @param name The option name.
   * @param value Receives the option value.
   * @param len The size of the value buffer, then the size of the value.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 GetOption(i32 level, i32 name, void* value, i32* len) const;

  /**
   * Reads the address of the connected peer.
   *
   * @param name Receives the peer address.
   * @param name_len The size of the address buffer, then the size of the peer address.
   * @return 0 on success, -1 with SocketError set.
   */
  REX_API i32 PeerName(sockaddr* name, i32* name_len) const;

  /**
   * Runs an ioctl request through SocketIoctl.
   *
   * @param cmd The platform ioctl request code.
   * @param arg The request argument buffer, read and written in place.
   * @return 0 on success, or the platform error return value.
   */
  i32 Ioctl(u32 cmd, u8* arg) const { return SocketIoctl(handle_, cmd, arg); }

  /**
   * Makes the socket nonblocking and binds an event to its waits through SocketPrepare.
   *
   * @param event A manual reset event whose Set ends a WaitSockets on the socket.
   * @return 0 on success, or the platform error return value.
   */
  i32 Prepare(WaitHandle* event) const { return SocketPrepare(handle_, event); }

 private:
  void Reset() {
    if (GetValid()) {
      SocketClose(std::exchange(handle_, kInvalidSocket));
    }
  }

  SocketHandle handle_ = kInvalidSocket;
};

}  // namespace rex::platform

/** Opts SocketReady into the flag operators. */
template <>
inline constexpr bool rex::is_flags_enum<rex::platform::SocketReady> = true;
