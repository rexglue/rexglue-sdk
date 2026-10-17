#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <algorithm>
#include <cerrno>
#include <utility>
#include <vector>

#include <poll.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rex::platform {

int SocketClose(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int SocketIoctl(SocketHandle handle, u32 cmd, u8* arg) {
  return ioctl(static_cast<int>(handle), cmd, arg);
}

bool SocketWouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

int PollSockets(std::span<const SocketWait> sockets, std::span<SocketReady> ready) {
  std::vector<pollfd> fds;
  fds.reserve(sockets.size());
  for (const SocketWait& socket : sockets) {
    short events = 0;
    if ((socket.ready & SocketReady::kRead) != SocketReady::kNone) {
      events |= POLLIN;
    }
    if ((socket.ready & SocketReady::kWrite) != SocketReady::kNone) {
      events |= POLLOUT;
    }
    if ((socket.ready & SocketReady::kExcept) != SocketReady::kNone) {
      events |= POLLPRI;
    }
    fds.push_back({static_cast<int>(socket.handle), events, 0});
  }
  int polled = 0;
  do {
    polled = poll(fds.data(), static_cast<nfds_t>(fds.size()), 0);
  } while (polled == -1 && errno == EINTR);
  if (polled == -1) {
    return -1;
  }
  int count = 0;
  for (size_t i = 0; i < fds.size(); ++i) {
    const short revents = fds[i].revents;
    SocketReady found = SocketReady::kNone;
    if ((fds[i].events & POLLIN) && (revents & (POLLIN | POLLHUP | POLLERR))) {
      found |= SocketReady::kRead;
    }
    if ((fds[i].events & POLLOUT) && (revents & POLLOUT)) {
      found |= SocketReady::kWrite;
    }
    if ((fds[i].events & POLLPRI) && (revents & (POLLPRI | POLLERR))) {
      found |= SocketReady::kExcept;
    }
    ready[i] = found;
    count += found != SocketReady::kNone ? 1 : 0;
  }
  return count;
}

u32 SocketError() {
  constexpr u32 kWSAEINVAL = 10022;
  constexpr std::pair<int, u32> kCodes[] = {
      {EINTR, 10004},         {EBADF, 10009},           {EACCES, 10013},
      {EFAULT, 10014},        {EINVAL, 10022},          {EMFILE, 10024},
      {EWOULDBLOCK, 10035},
#if EAGAIN != EWOULDBLOCK
      {EAGAIN, 10035},
#endif
      {EINPROGRESS, 10035},   {EALREADY, 10037},        {ENOTSOCK, 10038},
      {EDESTADDRREQ, 10039},  {EMSGSIZE, 10040},        {EPROTOTYPE, 10041},
      {ENOPROTOOPT, 10042},   {EPROTONOSUPPORT, 10043}, {ESOCKTNOSUPPORT, 10044},
      {EOPNOTSUPP, 10045},
#if ENOTSUP != EOPNOTSUPP
      {ENOTSUP, 10045},
#endif
      {EPFNOSUPPORT, 10046},  {EAFNOSUPPORT, 10047},    {EADDRINUSE, 10048},
      {EADDRNOTAVAIL, 10049}, {ENETDOWN, 10050},        {ENETUNREACH, 10051},
      {ENETRESET, 10052},     {ECONNABORTED, 10053},    {ECONNRESET, 10054},
      {ENOBUFS, 10055},       {EISCONN, 10056},         {ENOTCONN, 10057},
      {EPIPE, 10058},         {ESHUTDOWN, 10058},       {ETOOMANYREFS, 10059},
      {ETIMEDOUT, 10060},     {ECONNREFUSED, 10061},    {ELOOP, 10062},
      {ENAMETOOLONG, 10063},  {EHOSTDOWN, 10064},       {EHOSTUNREACH, 10065},
  };
  const auto* code = std::ranges::find(kCodes, errno, &std::pair<int, u32>::first);
  return code == std::ranges::end(kCodes) ? kWSAEINVAL : code->second;
}

namespace {

#if defined(MSG_NOSIGNAL)
constexpr i32 kSendFlags = MSG_NOSIGNAL;
#else
constexpr i32 kSendFlags = 0;
#endif

}  // namespace

Socket Socket::Create(i32 family, i32 type, i32 protocol) {
  return Socket(socket(family, type, protocol));
}

i32 Socket::Connect(const sockaddr* name, i32 name_len) const {
  return connect(static_cast<int>(handle_), name, static_cast<socklen_t>(name_len));
}

i32 Socket::Bind(const sockaddr* name, i32 name_len) const {
  return bind(static_cast<int>(handle_), name, static_cast<socklen_t>(name_len));
}

i32 Socket::GetName(sockaddr* name, i32* name_len) const {
  socklen_t length = static_cast<socklen_t>(*name_len);
  const int result = getsockname(static_cast<int>(handle_), name, &length);
  *name_len = static_cast<i32>(length);
  return result;
}

i32 Socket::Listen(i32 backlog) const {
  return listen(static_cast<int>(handle_), backlog);
}

Socket Socket::Accept(sockaddr* name, i32* name_len) const {
  socklen_t length = static_cast<socklen_t>(*name_len);
  const int accepted = accept(static_cast<int>(handle_), name, &length);
  *name_len = static_cast<i32>(length);
  return Socket(accepted);
}

i32 Socket::Shutdown(i32 how) const {
  return shutdown(static_cast<int>(handle_), how);
}

i32 Socket::Recv(void* buf, i32 len, i32 flags) const {
  return static_cast<i32>(recv(static_cast<int>(handle_), buf, static_cast<size_t>(len), flags));
}

i32 Socket::RecvFrom(void* buf, i32 len, i32 flags, sockaddr* from, i32* from_len) const {
  socklen_t length = static_cast<socklen_t>(*from_len);
  const ssize_t received =
      recvfrom(static_cast<int>(handle_), buf, static_cast<size_t>(len), flags, from, &length);
  *from_len = static_cast<i32>(length);
  return static_cast<i32>(received);
}

i32 Socket::Send(const void* buf, i32 len, i32 flags) const {
  return static_cast<i32>(
      send(static_cast<int>(handle_), buf, static_cast<size_t>(len), flags | kSendFlags));
}

i32 Socket::SendTo(const void* buf, i32 len, i32 flags, const sockaddr* to, i32 to_len) const {
  return static_cast<i32>(sendto(static_cast<int>(handle_), buf, static_cast<size_t>(len),
                                 flags | kSendFlags, to, static_cast<socklen_t>(to_len)));
}

i32 Socket::SetOption(i32 level, i32 name, const void* value, i32 len) const {
  return setsockopt(static_cast<int>(handle_), level, name, value, static_cast<socklen_t>(len));
}

i32 Socket::GetOption(i32 level, i32 name, void* value, i32* len) const {
  socklen_t length = static_cast<socklen_t>(*len);
  const int result = getsockopt(static_cast<int>(handle_), level, name, value, &length);
  *len = static_cast<i32>(length);
  return result;
}

i32 Socket::PeerName(sockaddr* name, i32* name_len) const {
  socklen_t length = static_cast<socklen_t>(*name_len);
  const int result = getpeername(static_cast<int>(handle_), name, &length);
  *name_len = static_cast<i32>(length);
  return result;
}

}  // namespace rex::platform
