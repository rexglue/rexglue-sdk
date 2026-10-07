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

#include <rex/macros.h>
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

}  // namespace rex::platform
