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

#include <rex/bit.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/types.h>

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
REX_API int socket_close(SocketHandle handle);

/**
 * Runs an ioctl request on a socket with ioctlsocket on Windows or ioctl on POSIX.
 *
 * @param handle The socket to control.
 * @param cmd The platform ioctl request code.
 * @param arg The request argument buffer, read and written in place.
 * @return 0 on success, or the platform error return value.
 */
REX_API int socket_ioctl(SocketHandle handle, u32 cmd, u8* arg);

/** The readiness a socket wait asks for. */
enum class SocketReady : u32 {
  kNone = 0,
  kRead = 1 << 0,   /**< Data, a connection or a peer close to read. */
  kWrite = 1 << 1,  /**< Space to send, or a finished connect. */
  kExcept = 1 << 2, /**< Out of band data, or a failed connect. */
};

/** A socket a wait watches. */
struct SocketWait {
  SocketHandle handle = kInvalidSocket;   /**< The socket. */
  WaitHandle* event = nullptr;            /**< The readiness event passed to socket_prepare. */
  SocketReady ready = SocketReady::kNone; /**< The readiness to wait for. */
};

/**
 * Makes a socket nonblocking and binds its readiness to an event on Windows.
 *
 * @param handle The socket.
 * @param event A manual reset event the socket sets when it becomes ready, unused on POSIX.
 * @return 0 on success, or the platform error return value.
 */
REX_API int socket_prepare(SocketHandle handle, WaitHandle* event);

/**
 * Reports whether the last socket call of the calling thread failed only because it would block.
 *
 * @return True when the error is a would block error or a connect in progress.
 */
REX_API bool socket_would_block();

/**
 * Waits until 1 socket may be ready.
 *
 * @param sockets The sockets to watch.
 * @param alertable True when a queued user callback ends the wait.
 * @param timeout The longest wait, milliseconds::max() for no limit.
 * @return kSuccess when a socket may be ready, kUserCallback, kTimeout or kFailed.
 */
REX_API WaitResult socket_wait(std::span<const SocketWait> sockets, bool alertable,
                               std::chrono::milliseconds timeout);

}  // namespace rex::platform

/** Opts SocketReady into the flag operators. */
template <>
// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr bool rex::is_flags_enum<rex::platform::SocketReady> = true;
