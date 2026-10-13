/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stop_token>
#include <thread>
#include <utility>

#include <rex/logging.h>
#include <rex/net/socket.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/socket_object.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

#include "runtime/thread_wait.h"

namespace rex::runtime {

constexpr u32 kMaxSocketLength = static_cast<u32>(std::numeric_limits<i32>::max());

/** An overlapped receive and the references it holds until a joiner releases them. */
struct PendingReceive {
  IObjectTable* table = nullptr;              /**< The table that owns the socket. */
  PinnedSlot pin;                             /**< The pin on the socket. */
  std::unique_ptr<rex::platform::Event> wake; /**< Set to end the wait of the thread. */
  ReceiveCompletion complete;                 /**< Called once with the outcome. */
  std::atomic<bool> completed = false;        /**< Set by the thread after complete. */
  std::jthread thread;                        /**< The thread that waits and receives. */
};

namespace {

using rex::platform::SocketReady;

constexpr u32 kFIONBIO = 0x8004667E;

int HostError() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

void SetHostError(int error) {
#if REX_PLATFORM_WIN32
  WSASetLastError(error);
#else
  errno = error;
#endif
}

#if REX_PLATFORM_WIN32
constexpr int kNoSocketsLeft = WSAEMFILE;
#else
constexpr int kNoSocketsLeft = EMFILE;
#endif

#if REX_PLATFORM_WIN32
void SetSendError(const SocketObject& /*sock*/) {}
#else
void SetSendError(const SocketObject& sock) {
  if (errno == EPIPE) {
    sockaddr_storage peer;
    i32 peer_len = sizeof(peer);
    errno =
        sock.socket.PeerName(reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0 ? EPIPE : ENOTCONN;
  }
}
#endif

rex::platform::SocketWait WaitOf(const SocketObject& sock, SocketReady ready) {
  return {sock.socket.handle(), sock.event.get(), ready};
}

bool Closed(const SocketObject& sock) {
  if (!sock.closed) {
    return false;
  }
  if (sock.event) {
    sock.event->Set();
  }
  return true;
}

bool WaitToRetry(const SocketObject& sock, SocketReady ready) {
  if (sock.nonblocking || !rex::platform::socket_would_block() || Closed(sock)) {
    return false;
  }
  const rex::platform::SocketWait wait = WaitOf(sock, ready);
  return WaitHostSockets({&wait, 1}, std::chrono::milliseconds::max()) ==
             rex::platform::WaitResult::kSuccess &&
         !Closed(sock);
}

bool WaitConnected(const SocketObject& sock) {
  const rex::platform::SocketWait wait = WaitOf(sock, SocketReady::kWrite | SocketReady::kExcept);
  while (!Closed(sock) &&
         WaitHostSockets({&wait, 1}, std::chrono::milliseconds::max()) ==
             rex::platform::WaitResult::kSuccess &&
         !Closed(sock)) {
    int error = 0;
    i32 error_len = sizeof(error);
    if (sock.socket.GetOption(SOL_SOCKET, SO_ERROR, &error, &error_len) != 0) {
      return false;
    }
    if (error != 0) {
      SetHostError(error);
      return false;
    }
    sockaddr_storage peer;
    i32 peer_len = sizeof(peer);
    if (sock.socket.PeerName(reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
      return true;
    }
  }
  return false;
}

sockaddr_in ToHost(const N_XSOCKADDR_IN& name) {
  sockaddr_in host{};
  host.sin_family = name.sin_family;
  host.sin_port = htons(name.sin_port);
  host.sin_addr.s_addr = htonl(name.sin_addr);
  return host;
}

sockaddr_in ToHost(const N_XSOCKADDR& name) {
  sockaddr_in host{};
  host.sin_family = name.address_family;
  std::memcpy(&host.sin_port, name.sa_data, sizeof(host.sin_port));
  std::memcpy(&host.sin_addr, name.sa_data + sizeof(host.sin_port), sizeof(host.sin_addr));
  return host;
}

void FromHost(const sockaddr_in& host, N_XSOCKADDR& name) {
  name.address_family = host.sin_family;
  std::memset(name.sa_data, 0, sizeof(name.sa_data));
  std::memcpy(name.sa_data, &host.sin_port, sizeof(host.sin_port));
  std::memcpy(name.sa_data + sizeof(host.sin_port), &host.sin_addr, sizeof(host.sin_addr));
}

void Finish(PendingReceive& receive, const ReceiveResult& result) {
  receive.complete(result);
  receive.completed.store(true, std::memory_order_release);
}

void ReceiveWhenReady(const std::stop_token& stop, const SocketObject& sock,
                      PendingReceive& receive, u8* buf, u32 buf_len, u32 flags) {
  const std::array waits{
      rex::platform::SocketWait{sock.socket.handle(), sock.event.get(), SocketReady::kRead},
      rex::platform::SocketWait{sock.socket.handle(), receive.wake.get(), SocketReady::kNone}};
  while (!stop.stop_requested() && !sock.closed) {
    ReceiveResult result = sock.TryRecvFrom(buf, buf_len, flags);
    if (!result.would_block) {
      Finish(receive, result);
      return;
    }
    if (rex::platform::socket_wait(waits, false, std::chrono::milliseconds::max()) ==
        rex::platform::WaitResult::kFailed) {
      result.would_block = false;
      result.error = rex::platform::socket_error();
      Finish(receive, result);
      return;
    }
  }
}

void Join(PendingReceive& receive) {
  receive.thread.request_stop();
  receive.wake->Set();
  receive.thread.join();
  if (!receive.completed.load(std::memory_order_acquire)) {
    ReceiveResult result;
    result.cancelled = true;
    receive.complete(result);
  }
  if (auto released = receive.table->ReleasePrimary(receive.pin); !released) {
    REXLOG_WARN(system, "A finished socket receive kept its reference, {}",
                released.error().message);
  }
}

X_STATUS Prepare(SocketObject& sock) {
  sock.event = rex::platform::Event::CreateManualResetEvent(false);
  if (!sock.event || sock.socket.Prepare(sock.event.get()) != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace

SocketObject::SocketObject(SocketAddressFamily af_in, SocketType type_in, SocketProtocol proto_in)
    : af(af_in), type(type_in), proto(proto_in) {}

SocketObject::~SocketObject() = default;

X_STATUS SocketObject::Initialize() {
  SocketProtocol native_proto = proto;
  if (native_proto == X_IPPROTO_VDP) {
    native_proto = X_IPPROTO_UDP;
  }

  socket = rex::platform::Socket::Create(af, type, native_proto);
  if (!socket.valid()) {
    return X_STATUS_UNSUCCESSFUL;
  }
#if defined(SO_NOSIGPIPE)
  const int enable = 1;
  if (setsockopt(sock.native_handle, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
#endif

  return Prepare(*this);
}

bool SocketObject::Cancel() {
  if (closed.exchange(true)) {
    return false;
  }
  if (event) {
    event->Set();
  }
  return true;
}

X_STATUS SocketObject::SetOption(u32 level, u32 optname, void* optval_ptr, u32 optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    secure = false;
    return X_STATUS_SUCCESS;
  }

  if (optlen > kMaxSocketLength) {
    return X_STATUS_INVALID_PARAMETER;
  }
  int ret = socket.SetOption(static_cast<i32>(level), static_cast<i32>(optname), optval_ptr,
                             static_cast<i32>(optlen));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::IOControl(u32 cmd, u8* arg_ptr) {
  if (cmd == kFIONBIO && arg_ptr) {
    u32 value = 0;
    std::memcpy(&value, arg_ptr, sizeof(value));
    nonblocking = value != 0;
    return X_STATUS_SUCCESS;
  }

  int ret = socket.Ioctl(cmd, arg_ptr);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Connect(N_XSOCKADDR* name, u32 name_len) const {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = socket.Connect(reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0 && (nonblocking || !rex::platform::socket_would_block() || !WaitConnected(*this))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Bind(N_XSOCKADDR_IN* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = socket.Bind(reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound = true;
  bound_port = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS SocketObject::Listen(int backlog) const {
  int ret = socket.Listen(backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_HANDLE SocketObject::Accept(Runtime& runtime, N_XSOCKADDR* name, int* name_len) {
  sockaddr_in n_sockaddr{};
  rex::platform::Socket connection;
  do {
    i32 n_name_len = sizeof(n_sockaddr);
    connection = socket.Accept(reinterpret_cast<sockaddr*>(&n_sockaddr), &n_name_len);
  } while (!connection.valid() && WaitToRetry(*this, SocketReady::kRead));
  if (!connection.valid()) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return X_INVALID_HANDLE_VALUE;
  }

  FromHost(n_sockaddr, *name);
  *name_len = sizeof(XSOCKADDR_IN);

  IObjectTable& table = runtime.object_table();
  Result<X_HANDLE> added = table.Emplace<SocketObject>(AddOptions{}, af, type, proto);
  if (!added) {
    connection = {};
    SetHostError(kNoSocketsLeft);
    return X_INVALID_HANDLE_VALUE;
  }
  const X_HANDLE handle = *added;
  auto accepted = table.Get<SocketObject>(handle);
  accepted->socket = std::move(connection);
  accepted->nonblocking = nonblocking;
  if (XFAILED(Prepare(*accepted))) {
    const int error = HostError();
    accepted = {};
    CloseOwned(table, handle);
    SetHostError(error);
    return X_INVALID_HANDLE_VALUE;
  }

  return handle;
}

int SocketObject::Shutdown(int how) const {
  return socket.Shutdown(how);
}

int SocketObject::Recv(u8* buf, u32 buf_len, u32 flags) const {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  int ret;
  do {
    ret = socket.Recv(buf, static_cast<i32>(buf_len), static_cast<i32>(flags));
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kRead));
  return ret;
}

int SocketObject::RecvFrom(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* from,
                           u32* from_len) const {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  sockaddr_in nfrom{};
  i32 nfromlen = sizeof(sockaddr_in);
  int ret;
  do {
    nfromlen = sizeof(sockaddr_in);
    ret = socket.RecvFrom(buf, static_cast<i32>(buf_len), static_cast<i32>(flags),
                          reinterpret_cast<sockaddr*>(&nfrom), &nfromlen);
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kRead));
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);
    from->sin_port = ntohs(nfrom.sin_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = static_cast<u32>(nfromlen);
  }

  return ret;
}

ReceiveResult SocketObject::TryRecvFrom(u8* buf, u32 buf_len, u32 flags) const {
  ReceiveResult result;
  sockaddr_in nfrom{};
  i32 nfromlen = sizeof(nfrom);
  result.received =
      socket.RecvFrom(buf, static_cast<i32>(std::min(buf_len, kMaxSocketLength)),
                      static_cast<i32>(flags), reinterpret_cast<sockaddr*>(&nfrom), &nfromlen);
  if (result.received == -1) {
    result.would_block = rex::platform::socket_would_block();
    result.error = rex::platform::socket_error();
    return result;
  }
  result.from.sin_family = nfrom.sin_family;
  result.from.sin_addr = ntohl(nfrom.sin_addr.s_addr);
  result.from.sin_port = ntohs(nfrom.sin_port);
  std::memset(result.from.x_sin_zero, 0, sizeof(result.from.x_sin_zero));
  result.from_len = static_cast<u32>(nfromlen);
  return result;
}

bool SocketObject::StartReceive(IObjectTable& table, PinnedSlot pin, u8* buf, u32 buf_len,
                                u32 flags, ReceiveCompletion complete) {
  std::lock_guard lock(receive_mutex);
  if (pending_receive) {
    if (!pending_receive->completed.load(std::memory_order_acquire)) {
      return false;
    }
    Join(*pending_receive);
    pending_receive.reset();
  }
  auto receive = std::make_unique<PendingReceive>();
  receive->wake = rex::platform::Event::CreateManualResetEvent(false);
  if (!receive->wake || !table.RetainPrimary(pin)) {
    return false;
  }
  receive->table = &table;
  receive->pin = std::move(pin);
  receive->complete = std::move(complete);
  PendingReceive& state = *receive;
  receive->thread = std::jthread([this, &state, buf, buf_len, flags](const std::stop_token& stop) {
    ReceiveWhenReady(stop, *this, state, buf, buf_len, flags);
  });
  pending_receive = std::move(receive);
  return true;
}

void SocketObject::CancelReceive() {
  std::lock_guard lock(receive_mutex);
  if (pending_receive) {
    Join(*pending_receive);
    pending_receive.reset();
  }
}

int SocketObject::Send(const u8* buf, u32 buf_len, u32 flags) const {
  if (buf_len > kMaxSocketLength) {
    return -1;
  }
  u32 sent = 0;
  while (true) {
    const int ret =
        socket.Send(buf + sent, static_cast<i32>(buf_len - sent), static_cast<i32>(flags));
    if (ret == -1) {
      if (WaitToRetry(*this, SocketReady::kWrite)) {
        continue;
      }
      SetSendError(*this);
      return sent > 0 ? static_cast<int>(sent) : -1;
    }
    sent += static_cast<u32>(ret);
    if (sent >= buf_len || nonblocking || type != X_SOCK_STREAM) {
      return static_cast<int>(sent);
    }
  }
}

int SocketObject::SendTo(u8* buf, u32 buf_len, u32 flags, N_XSOCKADDR_IN* to, u32 to_len) const {
  if ((to && to_len < sizeof(XSOCKADDR_IN)) || buf_len > kMaxSocketLength) {
    return -1;
  }
  const sockaddr_in nto = to ? ToHost(*to) : sockaddr_in{};

  int ret;
  do {
    ret = socket.SendTo(buf, static_cast<i32>(buf_len), static_cast<i32>(flags),
                        to ? reinterpret_cast<const sockaddr*>(&nto) : nullptr,
                        to ? static_cast<i32>(sizeof(nto)) : 0);
  } while (ret == -1 && WaitToRetry(*this, SocketReady::kWrite));
  if (ret == -1) {
    SetSendError(*this);
  }
  return ret;
}

}  // namespace rex::runtime
