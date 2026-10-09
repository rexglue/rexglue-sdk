#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <cerrno>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rex::platform {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, u32 cmd, u8* arg) {
  return ioctl(static_cast<int>(handle), cmd, arg);
}

int socket_prepare(SocketHandle handle, WaitHandle* /*event*/) {
  const int flags = fcntl(static_cast<int>(handle), F_GETFL);
  if (flags == -1) {
    return -1;
  }
#if defined(SO_NOSIGPIPE)
  const int enable = 1;
  if (setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) !=
      0) {
    return -1;
  }
#endif
  return fcntl(static_cast<int>(handle), F_SETFL, flags | O_NONBLOCK) == -1 ? -1 : 0;
}

bool socket_would_block() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

}  // namespace rex::platform
