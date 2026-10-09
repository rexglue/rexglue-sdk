#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <WinSock2.h>

#include <algorithm>
#include <vector>

namespace rex::platform {

int socket_close(SocketHandle handle) {
  return closesocket(static_cast<SOCKET>(handle));
}

int socket_ioctl(SocketHandle handle, u32 cmd, u8* arg) {
  return ioctlsocket(static_cast<SOCKET>(handle), cmd, reinterpret_cast<u_long*>(arg));
}

int socket_prepare(SocketHandle handle, WaitHandle* event) {
  return WSAEventSelect(static_cast<SOCKET>(handle), static_cast<WSAEVENT>(event->native_handle()),
                        FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE);
}

bool socket_would_block() {
  return WSAGetLastError() == WSAEWOULDBLOCK;
}

WaitResult socket_wait(std::span<const SocketWait> sockets, bool alertable,
                       std::chrono::milliseconds timeout) {
  std::vector<WaitHandle*> events;
  for (const SocketWait& socket : sockets) {
    if (std::ranges::find(events, socket.event) == events.end()) {
      events.push_back(socket.event);
    }
  }
  const auto [result, index] =
      WaitMultiple(events.data(), events.size(), false, alertable, timeout);
  if (result == WaitResult::kSuccess) {
    for (const SocketWait& socket : sockets) {
      WSANETWORKEVENTS network_events;
      WSAEnumNetworkEvents(static_cast<SOCKET>(socket.handle),
                           static_cast<WSAEVENT>(socket.event->native_handle()), &network_events);
    }
  }
  return result;
}

}  // namespace rex::platform
