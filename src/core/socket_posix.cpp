#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  return ioctl(static_cast<int>(handle), cmd, arg);
}

int socket_set_non_blocking(SocketHandle handle, bool non_blocking) {
  const int fd = static_cast<int>(handle);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(fd, F_SETFL, flags);
}

}  // namespace rex::net
