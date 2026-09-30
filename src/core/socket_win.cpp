#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <WinSock2.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return closesocket(static_cast<SOCKET>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  return ioctlsocket(static_cast<SOCKET>(handle), cmd, reinterpret_cast<u_long*>(arg));
}

int socket_set_non_blocking(SocketHandle handle, bool non_blocking) {
  u_long value = non_blocking ? 1 : 0;
  return ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &value);
}

}  // namespace rex::net
