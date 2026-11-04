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

#include "xam/xam_state.h"
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

#include <rex/logging/log_category.h>
#include <rex/memory/endian.h>
#include <rex/platform/socket.h>
#include <rex/platform.h>
#include "xam/socket_object.h"

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/platform/sync.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace rex::xam {

/** An overlapped receive and the references it holds until a joiner releases them. */
struct PendingReceive {
  kernel::IObjectTable* table = nullptr;      /**< The table that owns the socket. */
  kernel::ObjectPin pin;                      /**< The pin on the socket. */
  std::unique_ptr<rex::platform::Event> wake; /**< Set to end the wait of the thread. */
  ReceiveCompletion complete;                 /**< Called once with the outcome. */
  std::atomic<bool> completed = false;        /**< Set by the thread after complete. */
  std::jthread thread;                        /**< The thread that waits and receives. */
};

namespace {

using rex::platform::SocketReady;

constexpr u32 kFIONBIO = 0x8004667E;

int GetHostError() {
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
    errno = sock.socket.GetPeerName(reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0 ? EPIPE
                                                                                        : ENOTCONN;
  }
}
#endif

rex::platform::SocketWait WaitOf(const SocketObject& sock, SocketReady ready) {
  return {sock.socket.GetHandle(), sock.event.get(), ready};
}

kernel::IObjectWaiter& HostWait() {
  return CurrentTitle().threads.GetObjectWaiter();
}

bool IsClosed(const SocketObject& sock) {
  if (!sock.closed) {
    return false;
  }
  if (sock.event) {
    sock.event->Set();
  }
  return true;
}

bool WaitToRetry(const SocketObject& sock, SocketReady ready) {
  if (sock.nonblocking || !rex::platform::IsSocketWouldBlock() || IsClosed(sock)) {
    return false;
  }
  const rex::platform::SocketWait wait = WaitOf(sock, ready);
  return HostWait().WaitHostSockets({&wait, 1}, std::chrono::milliseconds::max()) ==
             rex::platform::WaitResult::kSuccess &&
         !IsClosed(sock);
}

bool WaitConnected(const SocketObject& sock) {
  const rex::platform::SocketWait wait = WaitOf(sock, SocketReady::kWrite | SocketReady::kExcept);
  while (!IsClosed(sock) &&
         HostWait().WaitHostSockets({&wait, 1}, std::chrono::milliseconds::max()) ==
             rex::platform::WaitResult::kSuccess &&
         !IsClosed(sock)) {
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
    if (sock.socket.GetPeerName(reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
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
      rex::platform::SocketWait{sock.socket.GetHandle(), sock.event.get(), SocketReady::kRead},
      rex::platform::SocketWait{sock.socket.GetHandle(), receive.wake.get(), SocketReady::kNone}};
  while (!stop.stop_requested() && !sock.closed) {
    ReceiveResult result = sock.TryRecvFrom(buf, buf_len, flags);
    if (!result.would_block) {
      Finish(receive, result);
      return;
    }
    if (rex::platform::WaitSockets(waits, false, std::chrono::milliseconds::max()) ==
        rex::platform::WaitResult::kFailed) {
      result.would_block = false;
      result.error = rex::platform::GetSocketError();
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
    result.canceled = true;
    receive.complete(result);
  }
  if (auto released = receive.table->Dereference(receive.pin); !released) {
    REXSYS_WARN("A finished socket receive kept its reference, {}", released.error().message);
  }
}

kernel::X_STATUS Prepare(SocketObject& sock) {
  sock.event = rex::platform::Event::CreateManualResetEvent(false);
  if (!sock.event || sock.socket.Prepare(sock.event.get()) != 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }
  return kernel::X_STATUS_SUCCESS;
}

void DisableNagle(SocketObject& object) {
  if (object.type == kXSockStream) {
    const i32 no_delay = 1;
    object.socket.SetOption(IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
  }
}

}  // namespace

SocketObject::SocketObject(SocketAddressFamily af_in, SocketType type_in, SocketProtocol proto_in)
    : af(af_in), type(type_in), proto(proto_in) {}

SocketObject::~SocketObject() = default;

kernel::X_STATUS SocketObject::Initialize() {
  SocketProtocol native_proto = proto;
  if (native_proto == kXIPProtoVDP) {
    native_proto = kXIPProtoUDP;
  }

  socket = rex::platform::Socket::Create(af, type, native_proto);
  if (!socket.IsValid()) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  DisableNagle(*this);

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

namespace {

enum class OptionType : u8 { kInt, kBool, kLinger, kDontLinger, kTimeout, kExclusive, kNoDelay };

struct OptionRule {
  u32 level;
  u32 name;
  OptionType type;
  bool read_only;
  i32 host_level;
  i32 host_name;
};

constexpr u32 kGuestSolSocket = 0xFFFF;
constexpr u32 kGuestProtocolTCP = 6;
constexpr u32 kGuestOptionSize = 4;

#if REX_PLATFORM_WIN32
using HostLinger = LINGER;
constexpr i32 kHostExclusive = SO_EXCLUSIVEADDRUSE;
#else
using HostLinger = struct linger;
constexpr i32 kHostExclusive = 0;
#endif

constexpr OptionRule kOptionRules[] = {
    {kGuestSolSocket, 0x0002, OptionType::kBool, true, SOL_SOCKET, SO_ACCEPTCONN},
    {kGuestSolSocket, 0x0004, OptionType::kBool, false, SOL_SOCKET, SO_REUSEADDR},
    {kGuestSolSocket, 0x0020, OptionType::kBool, false, SOL_SOCKET, SO_BROADCAST},
    {kGuestSolSocket, 0x0080, OptionType::kLinger, false, SOL_SOCKET, SO_LINGER},
    {kGuestSolSocket, ~static_cast<u32>(0x0080), OptionType::kDontLinger, false, SOL_SOCKET,
     SO_LINGER},
    {kGuestSolSocket, ~static_cast<u32>(0x0004), OptionType::kExclusive, false, SOL_SOCKET,
     kHostExclusive},
    {kGuestSolSocket, 0x1001, OptionType::kInt, false, SOL_SOCKET, SO_SNDBUF},
    {kGuestSolSocket, 0x1002, OptionType::kInt, false, SOL_SOCKET, SO_RCVBUF},
    {kGuestSolSocket, 0x1005, OptionType::kTimeout, false, SOL_SOCKET, SO_SNDTIMEO},
    {kGuestSolSocket, 0x1006, OptionType::kTimeout, false, SOL_SOCKET, SO_RCVTIMEO},
    {kGuestSolSocket, 0x1008, OptionType::kInt, true, SOL_SOCKET, SO_TYPE},
    {kGuestProtocolTCP, 0x0001, OptionType::kNoDelay, false, IPPROTO_TCP, TCP_NODELAY},
};

bool IsGuestLevel(u32 level) {
  return level == kGuestSolSocket || level == kGuestProtocolTCP;
}

const OptionRule* FindOption(u32 level, u32 optname) {
  for (const OptionRule& rule : kOptionRules) {
    if (rule.level == level && rule.name == optname) {
      return &rule;
    }
  }
  return nullptr;
}

#if REX_PLATFORM_WIN32
using HostTimeout = DWORD;

HostTimeout ToHostTimeout(u32 milliseconds) {
  return milliseconds;
}

u32 FromHostTimeout(HostTimeout timeout) {
  return timeout;
}
#else
using HostTimeout = timeval;

HostTimeout ToHostTimeout(u32 milliseconds) {
  timeval timeout{};
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(milliseconds / 1000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((milliseconds % 1000) * 1000);
  return timeout;
}

u32 FromHostTimeout(const HostTimeout& timeout) {
  return static_cast<u32>(timeout.tv_sec) * 1000 + static_cast<u32>(timeout.tv_usec) / 1000;
}
#endif

}  // namespace

kernel::X_STATUS SocketObject::SetOption(u32 level, u32 optname, void* optval_ptr, u32 optlen) {
  if (level == kGuestSolSocket && (optname == 0x5801 || optname == 0x5802)) {
    secure = false;
    return kernel::X_STATUS_SUCCESS;
  }

  if (optlen > kMaxSocketLength) {
    return kernel::X_STATUS_INVALID_PARAMETER;
  }
  if (!IsGuestLevel(level)) {
    return kernel::X_STATUS_INVALID_PARAMETER_1;
  }
  const OptionRule* rule = FindOption(level, optname);
  if (!rule || rule->read_only) {
    return kernel::X_STATUS_NOT_SUPPORTED;
  }
  if (optlen < kGuestOptionSize) {
    return kernel::X_STATUS_BUFFER_TOO_SMALL;
  }

  const u32 word = memory::load<be_u32>(static_cast<const u8*>(optval_ptr));
  int ret = 0;
  switch (rule->type) {
    case OptionType::kInt: {
      const i32 value = static_cast<i32>(word);
      ret = socket.SetOption(rule->host_level, rule->host_name, &value, sizeof(value));
      break;
    }
    case OptionType::kBool: {
      const i32 value = word != 0 ? 1 : 0;
      ret = socket.SetOption(rule->host_level, rule->host_name, &value, sizeof(value));
      break;
    }
    case OptionType::kNoDelay:
      break;
    case OptionType::kLinger:
    case OptionType::kDontLinger: {
      HostLinger value{};
      if (rule->type == OptionType::kLinger) {
        value.l_onoff = static_cast<decltype(value.l_onoff)>(word >> 16);
        value.l_linger = static_cast<decltype(value.l_linger)>(word & 0xFFFF);
      } else {
        value.l_onoff = word != 0 ? 0 : 1;
        value.l_linger = 0;
      }
      ret = socket.SetOption(rule->host_level, rule->host_name, &value, sizeof(value));
      break;
    }
    case OptionType::kTimeout: {
      const HostTimeout value = ToHostTimeout(word);
      ret = socket.SetOption(rule->host_level, rule->host_name, &value, sizeof(value));
      break;
    }
    case OptionType::kExclusive: {
#if REX_PLATFORM_WIN32
      const i32 value = word != 0 ? 1 : 0;
      ret = socket.SetOption(rule->host_level, rule->host_name, &value, sizeof(value));
#endif
      break;
    }
  }
  if (ret < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::GetOption(u32 level, u32 optname, void* optval_ptr,
                                         u32* optlen) const {
  if (!IsGuestLevel(level)) {
    return kernel::X_STATUS_INVALID_PARAMETER_1;
  }
  const OptionRule* rule = FindOption(level, optname);
  if (!rule) {
    return kernel::X_STATUS_NOT_SUPPORTED;
  }
  if (*optlen < kGuestOptionSize) {
    return kernel::X_STATUS_BUFFER_TOO_SMALL;
  }

  u32 word = 0;
  int ret = 0;
  switch (rule->type) {
    case OptionType::kNoDelay:
      word = 1;
      break;
    case OptionType::kInt:
    case OptionType::kBool: {
      i32 value = 0;
      i32 length = sizeof(value);
      ret = socket.GetOption(rule->host_level, rule->host_name, &value, &length);
      word = rule->type == OptionType::kInt ? static_cast<u32>(value) : (value != 0 ? 1u : 0u);
      break;
    }
    case OptionType::kLinger:
    case OptionType::kDontLinger: {
      HostLinger value{};
      i32 length = sizeof(value);
      ret = socket.GetOption(rule->host_level, rule->host_name, &value, &length);
      if (rule->type == OptionType::kLinger) {
        word = (static_cast<u32>(value.l_onoff != 0 ? 1 : 0) << 16) |
               (static_cast<u32>(value.l_linger) & 0xFFFF);
      } else {
        word = value.l_onoff != 0 ? 0 : 1;
      }
      break;
    }
    case OptionType::kTimeout: {
      HostTimeout value{};
      i32 length = sizeof(value);
      ret = socket.GetOption(rule->host_level, rule->host_name, &value, &length);
      word = FromHostTimeout(value);
      break;
    }
    case OptionType::kExclusive: {
#if REX_PLATFORM_WIN32
      i32 value = 0;
      i32 length = sizeof(value);
      ret = socket.GetOption(rule->host_level, rule->host_name, &value, &length);
      word = value != 0 ? 1 : 0;
#endif
      break;
    }
  }
  if (ret < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  memory::store<be_u32>(static_cast<u8*>(optval_ptr), word);
  *optlen = kGuestOptionSize;
  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::IOControl(u32 cmd, u8* arg_ptr) {
  if (cmd == kFIONBIO && arg_ptr) {
    u32 value = 0;
    std::memcpy(&value, arg_ptr, sizeof(value));
    nonblocking = value != 0;
    return kernel::X_STATUS_SUCCESS;
  }

  int ret = socket.IOCTL(cmd, arg_ptr);
  if (ret < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::Connect(N_XSOCKADDR* name, u32 name_len) const {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = socket.Connect(reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0 && (nonblocking || !rex::platform::IsSocketWouldBlock() || !WaitConnected(*this))) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::Bind(N_XSOCKADDR_IN* name, u32 name_len) {
  if (name_len < sizeof(XSOCKADDR_IN) || name_len > kMaxSocketLength) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }
  const sockaddr_in host = ToHost(*name);
  int ret = socket.Bind(reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  if (ret < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::GetName(N_XSOCKADDR* name) const {
  sockaddr_in host{};
  i32 host_len = sizeof(host);
  if (socket.GetLocalName(reinterpret_cast<sockaddr*>(&host), &host_len) < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }
  if (host.sin_port == 0) {
    return kernel::X_STATUS_INVALID_PARAMETER_1;
  }
  host.sin_addr.s_addr = 0;
  FromHost(host, *name);
  return kernel::X_STATUS_SUCCESS;
}

kernel::X_STATUS SocketObject::Listen(int backlog) const {
  int ret = socket.Listen(backlog);
  if (ret < 0) {
    return kernel::X_STATUS_UNSUCCESSFUL;
  }

  return kernel::X_STATUS_SUCCESS;
}

kernel::X_HANDLE SocketObject::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr_in n_sockaddr{};
  rex::platform::Socket connection;
  do {
    i32 n_name_len = sizeof(n_sockaddr);
    connection = socket.Accept(reinterpret_cast<sockaddr*>(&n_sockaddr), &n_name_len);
  } while (!connection.IsValid() && WaitToRetry(*this, SocketReady::kRead));
  if (!connection.IsValid()) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return kernel::kXInvalidHandleValue;
  }

  FromHost(n_sockaddr, *name);
  *name_len = sizeof(XSOCKADDR_IN);

  kernel::IObjectTable& table = CurrentTitle().objects;
  Result<kernel::X_HANDLE> added =
      table.Emplace<SocketObject>(kernel::AddOptions{}, af, type, proto);
  if (!added) {
    connection = {};
    SetHostError(kNoSocketsLeft);
    return kernel::kXInvalidHandleValue;
  }
  const kernel::X_HANDLE handle = *added;
  auto accepted = table.Get<SocketObject>(handle);
  accepted->socket = std::move(connection);
  accepted->nonblocking = nonblocking;
  DisableNagle(*accepted);
  if (kernel::XFAILED(Prepare(*accepted))) {
    const int error = GetHostError();
    accepted = {};
    kernel::CloseOwned(table, handle);
    SetHostError(error);
    return kernel::kXInvalidHandleValue;
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
    result.would_block = rex::platform::IsSocketWouldBlock();
    result.error = rex::platform::GetSocketError();
    return result;
  }
  result.from.sin_family = nfrom.sin_family;
  result.from.sin_addr = ntohl(nfrom.sin_addr.s_addr);
  result.from.sin_port = ntohs(nfrom.sin_port);
  std::memset(result.from.x_sin_zero, 0, sizeof(result.from.x_sin_zero));
  result.from_len = static_cast<u32>(nfromlen);
  return result;
}

bool SocketObject::StartReceive(kernel::IObjectTable& table, kernel::ObjectPin pin, u8* buf,
                                u32 buf_len, u32 flags, ReceiveCompletion complete) {
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
  if (!receive->wake || !table.Reference(pin)) {
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
    if (sent >= buf_len || nonblocking || type != kXSockStream) {
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

void ShutdownSockets(kernel::IObjectTable& table) {
  for (auto& sock : table.All<SocketObject>()) {
    sock->CancelReceive();
    sock->Shutdown(2);
  }
}

}  // namespace rex::xam
