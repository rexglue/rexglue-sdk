/**
 * @file        net/socket.h
 * @brief       Platform-agnostic socket operations
 */
#pragma once

#include <cstdint>

namespace rex::net {

using SocketHandle = int64_t;
constexpr SocketHandle kInvalidSocket = -1;

int socket_close(SocketHandle handle);
int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg);

// Blocking mode is the one socket option whose control differs per platform:
// Winsock spells it ioctlsocket(FIONBIO), POSIX spells it fcntl(O_NONBLOCK),
// and the two FIONBIO constants are not the same value. Callers should use
// this rather than passing a hardcoded FIONBIO through socket_ioctl.
int socket_set_non_blocking(SocketHandle handle, bool non_blocking);

}  // namespace rex::net
