#include <rex/platform/socket.h>
#include <rex/platform.h>
#include <rex/platform/sync.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <WinSock2.h>

#include <algorithm>
#include <vector>

namespace rex::platform {

int SocketClose(SocketHandle handle) {
  return closesocket(static_cast<SOCKET>(handle));
}

int SocketIOCTL(SocketHandle handle, u32 cmd, u8* arg) {
  return ioctlsocket(static_cast<SOCKET>(handle), static_cast<long>(cmd),
                     reinterpret_cast<u_long*>(arg));
}

int SocketPrepare(SocketHandle handle, WaitHandle* event) {
  return WSAEventSelect(static_cast<SOCKET>(handle),
                        static_cast<WSAEVENT>(event->GetNativeHandle()),
                        FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE);
}

int PollSockets(std::span<const SocketWait> sockets, std::span<SocketReady> ready) {
  std::vector<WSAPOLLFD> fds;
  fds.reserve(sockets.size());
  for (const SocketWait& socket : sockets) {
    SHORT events = 0;
    if ((socket.ready & SocketReady::kRead) != SocketReady::kNone) {
      events |= POLLRDNORM;
    }
    if ((socket.ready & SocketReady::kWrite) != SocketReady::kNone) {
      events |= POLLWRNORM;
    }
    if ((socket.ready & SocketReady::kExcept) != SocketReady::kNone) {
      events |= POLLRDBAND;
    }
    fds.push_back({static_cast<SOCKET>(socket.handle), events, 0});
  }
  if (WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 0) == SOCKET_ERROR) {
    return -1;
  }
  int count = 0;
  for (size_t i = 0; i < fds.size(); ++i) {
    const SHORT revents = fds[i].revents;
    SocketReady found = SocketReady::kNone;
    if ((fds[i].events & POLLRDNORM) && (revents & (POLLRDNORM | POLLHUP | POLLERR))) {
      found |= SocketReady::kRead;
    }
    if ((fds[i].events & POLLWRNORM) && (revents & POLLWRNORM)) {
      found |= SocketReady::kWrite;
    }
    if ((fds[i].events & POLLRDBAND) && (revents & (POLLRDBAND | POLLERR))) {
      found |= SocketReady::kExcept;
    }
    ready[i] = found;
    count += found != SocketReady::kNone ? 1 : 0;
  }
  return count;
}

bool IsSocketWouldBlock() {
  return WSAGetLastError() == WSAEWOULDBLOCK;
}

u32 GetSocketError() {
  return static_cast<u32>(WSAGetLastError());
}

WaitResult WaitSockets(std::span<const SocketWait> sockets, bool alertable,
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
                           static_cast<WSAEVENT>(socket.event->GetNativeHandle()), &network_events);
    }
  }
  return result;
}

Socket Socket::Create(i32 family, i32 type, i32 protocol) {
  const SOCKET created = socket(family, type, protocol);
  return Socket(created == INVALID_SOCKET ? kInvalidSocket : static_cast<SocketHandle>(created));
}

i32 Socket::Connect(const sockaddr* name, i32 name_len) const {
  return connect(static_cast<SOCKET>(handle_), name, name_len);
}

i32 Socket::Bind(const sockaddr* name, i32 name_len) const {
  return bind(static_cast<SOCKET>(handle_), name, name_len);
}

i32 Socket::GetLocalName(sockaddr* name, i32* name_len) const {
  return getsockname(static_cast<SOCKET>(handle_), name, name_len);
}

i32 Socket::Listen(i32 backlog) const {
  return listen(static_cast<SOCKET>(handle_), backlog);
}

Socket Socket::Accept(sockaddr* name, i32* name_len) const {
  const SOCKET accepted = accept(static_cast<SOCKET>(handle_), name, name_len);
  return Socket(accepted == INVALID_SOCKET ? kInvalidSocket : static_cast<SocketHandle>(accepted));
}

i32 Socket::Shutdown(i32 how) const {
  return shutdown(static_cast<SOCKET>(handle_), how);
}

i32 Socket::Recv(void* buf, i32 len, i32 flags) const {
  return recv(static_cast<SOCKET>(handle_), static_cast<char*>(buf), len, flags);
}

i32 Socket::RecvFrom(void* buf, i32 len, i32 flags, sockaddr* from, i32* from_len) const {
  return recvfrom(static_cast<SOCKET>(handle_), static_cast<char*>(buf), len, flags, from,
                  from_len);
}

i32 Socket::Send(const void* buf, i32 len, i32 flags) const {
  return send(static_cast<SOCKET>(handle_), static_cast<const char*>(buf), len, flags);
}

i32 Socket::SendTo(const void* buf, i32 len, i32 flags, const sockaddr* to, i32 to_len) const {
  return sendto(static_cast<SOCKET>(handle_), static_cast<const char*>(buf), len, flags, to,
                to_len);
}

i32 Socket::SetOption(i32 level, i32 name, const void* value, i32 len) const {
  return setsockopt(static_cast<SOCKET>(handle_), level, name, static_cast<const char*>(value),
                    len);
}

i32 Socket::GetOption(i32 level, i32 name, void* value, i32* len) const {
  return getsockopt(static_cast<SOCKET>(handle_), level, name, static_cast<char*>(value), len);
}

i32 Socket::GetPeerName(sockaddr* name, i32* name_len) const {
  return getpeername(static_cast<SOCKET>(handle_), name, name_len);
}

}  // namespace rex::platform
