/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <rex/chrono.h>
#include "xam/xam_ui.h"
#include <rex/kernel/xboxkrnl.h>
#include <rex/logging/log.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/platform/socket.h>
#include <rex/ppc/hook.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/string/text.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/object.h>
#include "xam/socket_object.h"
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/heap.h>
#include "xam/argument_check.h"
#include "xam/stub_log.h"
#include "xam/xam_bodies.h"
#include "xam/xam_state.h"

#if REX_PLATFORM_WIN32
// Must be included last, it expects windows.h to be included before it.
// NOLINTNEXTLINE(bugprone-reserved-identifier): Windows SDK macro name
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace xam {

constexpr u32 kWSAEINTR = 0x2714;
constexpr u32 kWSAEFAULT = 0x271E;
constexpr u32 kWSAEINVAL = 0x2726;
constexpr u32 kWSAENOTSOCK = 0x2736;
constexpr u32 kWSAENOPROTOOPT = 0x277A;
constexpr u32 kWSAEWOULDBLOCK = 0x2733;
constexpr u32 kWSAECANCELLED = 0x2777;
constexpr u32 kWSAIOIncomplete = 996;
constexpr u32 kWSAIOPending = 997;
using namespace rex::kernel;

namespace {

kernel::ICallingThread& CurrentThread() {
  return *CurrentTitle().threads.GetCallingThread();
}

Pinned<SocketObject> GetSocket(u32 socket_handle) {
  auto socket = CurrentTitle().objects.Get<SocketObject>(socket_handle);
  if (!socket || socket->closed) {
    CurrentThread().SetLastError(kWSAENOTSOCK);
    return {};
  }
  return socket;
}

u32 CallError(const SocketObject& socket) {
  return socket.closed ? kWSAEINTR : rex::platform::GetSocketError();
}

u32 OptionError(const SocketObject& socket, kernel::X_STATUS status) {
  switch (status) {
    case kernel::X_STATUS_NOT_SUPPORTED:
      return kWSAENOPROTOOPT;
    case kernel::X_STATUS_INVALID_PARAMETER_1:
      return kWSAEINVAL;
    case kernel::X_STATUS_BUFFER_TOO_SMALL:
    case kernel::X_STATUS_INVALID_PARAMETER:
      return kWSAEFAULT;
    default:
      return CallError(socket);
  }
}

void SetCallError(const SocketObject& socket) {
  CurrentThread().SetLastError(CallError(socket));
}

bool IsValidNameLength(u32 name_len) {
  return name_len >= sizeof(XSOCKADDR_IN) && name_len <= kMaxSocketLength;
}

bool InGuestRange(kernel::ISystemHeap& heap, u32 address, u32 length) {
  if (length == 0) {
    return true;
  }
  const u32 last = address + length - 1;
  return address != 0 && last >= address && heap.IsAddressMapped(address) &&
         heap.IsAddressMapped(last);
}

void StoreFrom(GuestPtrT<XSOCKADDR_IN> from_ptr, MappedU32 from_len, const ReceiveResult& result) {
  if (!from_ptr || !from_len) {
    return;
  }
  XSOCKADDR_IN from{};
  from.sin_family = result.from.sin_family;
  from.sin_port = result.from.sin_port;
  from.sin_addr = result.from.sin_addr;
  std::memcpy(from_ptr.GetHostPtr(), &from, std::min<u32>(*from_len, sizeof(from)));
  *from_len = result.from_len;
}

kernel::X_STATUS SetGuestEvent(kernel::X_HANDLE handle) {
  auto event = CurrentTitle().objects.Get<kernel::IGuestEvent>(handle);
  if (!event) {
    return kernel::X_STATUS_INVALID_HANDLE;
  }
  event->Set();
  return kernel::X_STATUS_SUCCESS;
}

}  // namespace

enum : u8 {
  kXNCallerInvalid = 0x0,
  kXNCallerTitle = 0x1,
  kXNCallerSysApp = 0x2,
  kXNCallerXBDM = 0x3,
  kXNCallerTest = 0x4,
  kNumXNCallerTypes = 0x4,
};

struct XNADDR {
  // IN_ADDR fields are in network byte order.
  in_addr ina;       /**< IP address, zero unless static or DHCP. */
  in_addr inaOnline; /**< Online IP address, zero when not online. */
  be_u16 wPortOnline;
  u8 abEnet[6];
  u8 abOnline[20];
};

struct XNDNS {
  be_i32 status;
  be_u32 cina;
  in_addr aina[8];
};

struct XNQOSINFO {
  u8 flags;
  u8 reserved;
  be_u16 xmit_count;
  be_u16 recv_count;
  be_u16 data_len;
  be_u32 data_ptr;
  be_u16 rtt_min_in_msecs;
  be_u16 rtt_med_in_msecs;
  be_u32 up_bits_per_sec;
  be_u32 down_bits_per_sec;
};

struct XNQOS {
  be_u32 count;
  be_u32 count_pending;
  XNQOSINFO info[1];
};

struct Xsockaddr_t {
  be_u16 sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  be_u16 version;
  be_u16 version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  be_u16 max_sockets;
  be_u16 max_udpdg;
  be_u32 vendor_info_ptr;
};

struct XWSABUF {
  be_u32 len;
  be_u32 buf_ptr;
};

struct XWSAOVERLAPPED {
  be_u32 internal;
  be_u32 internal_high;
  union {
    struct {
      be_u32 low;
      be_u32 high;
    } offset;  // Named because GCC rejects members with constructors in an anonymous struct.
    be_u32 pointer;
  };
  be_u32 event_handle;
};

struct XNetStartupParams {
  u8 cfgSizeOfStruct;
  u8 cfgFlags;
  u8 cfgSockMaxDgramSockets;
  u8 cfgSockMaxStreamSockets;
  u8 cfgSockDefaultRecvBufsizeInK;
  u8 cfgSockDefaultSendBufsizeInK;
  u8 cfgKeyRegMax;
  u8 cfgSecRegMax;
  u8 cfgQosDataLimitDiv4;
  u8 cfgQosProbeTimeoutInSeconds;
  u8 cfgQosProbeRetries;
  u8 cfgQosSrvMaxSimultaneousResponses;
  u8 cfgQosPairWaitTimeInSeconds;
};

static_assert(sizeof(XNetStartupParams) == 13);

u32 NetDll_XNetStartup_entry([[maybe_unused]] u32 caller, GuestPtrT<XNetStartupParams> params) {
  if (params) {
    REX_ASSERT(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(CurrentXAM().GetNetStartupParams().data(), params, sizeof(XNetStartupParams));
  }

  return 0;
}

u32 NetDll_XNetCleanup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] MappedVoid params) {
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, MappedVoid buffer_ptr, MappedU32 buffer_size) {
  REX_ASSERT(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        // WSAEMSGSIZE
        return 0x2738;
      }
      std::memcpy(buffer_ptr, CurrentXAM().GetNetStartupParams().data(), sizeof(XNetStartupParams));
      return 0;
    default:
      REX_XAM_STUB("NetDll_XNetGetOpt: option {}", option_id);
      // WSAEINVAL
      return 0x2726;
  }
}

u32 NetDll_XNetRandom_entry([[maybe_unused]] u32 caller, MappedVoid buffer_ptr, u32 length) {
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry([[maybe_unused]] u32 caller, u16 version,
                            GuestPtrT<X_WSADATA> data_ptr) {
#if REX_PLATFORM_WIN32
  WSADATA wsa_data;
  ZeroMemory(&wsa_data, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsa_data);

  if (data_ptr) {
    data_ptr->version = wsa_data.wVersion;
    data_ptr->version_high = wsa_data.wHighVersion;
    std::memcpy(&data_ptr->description, wsa_data.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsa_data.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsa_data.iMaxSockets;
    data_ptr->max_udpdg = wsa_data.iMaxUdpDg;
  }
#else
  int ret = 0;
  if (data_ptr) {
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  return ret;
}

u32 NetDll_WSACleanup_entry([[maybe_unused]] u32 caller) {
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return CurrentThread().GetLastError();
}

namespace {

void CompleteReceive(GuestPtrT<XWSAOVERLAPPED> overlapped, GuestPtrT<XSOCKADDR_IN> from_ptr,
                     MappedU32 from_len, u32 event_handle, const ReceiveResult& result) {
  u32 status = 0;
  if (result.canceled) {
    status = kWSAECANCELLED;
    overlapped->internal_high = 0;
  } else if (result.received == -1) {
    status = result.error;
    overlapped->internal_high = 0;
  } else {
    overlapped->internal_high = static_cast<u32>(result.received);
    StoreFrom(from_ptr, from_len, result);
  }
  CurrentTitle().overlapped_dispatcher.Publish([&] {
    overlapped->internal.Store(status, std::memory_order_release);
    if (event_handle) {
      if (auto event = CurrentTitle().objects.Get<kernel::IGuestEvent>(event_handle)) {
        event->Set();
      }
    }
  });
}

}  // namespace

/**
 * Receives a datagram into buffer 0, at once or overlapped.
 *
 * @param socket_handle The socket to receive on.
 * @param buffers The buffers, only the first is used.
 * @param buffer_count Ignored.
 * @param num_bytes_recv Receives the byte count of a receive that completes at once.
 * @param flags_ptr Receives 0 for a receive that completes at once.
 * @param from_ptr Receives the sender, at most *from_len bytes of it.
 * @param from_len The size of the sender buffer, receives the size of the sender address.
 * @param overlapped Optional overlapped block that receives the result, and whose event is set.
 * @param completion_routine Ignored.
 * @return 0 when the receive completed, -1 with the last error set otherwise, kWSAIOPending for
 * a receive that completes later.
 */
u32 NetDll_WSARecvFrom_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                             GuestPtrT<XWSABUF> buffers, [[maybe_unused]] u32 buffer_count,
                             MappedU32 num_bytes_recv, MappedU32 flags_ptr,
                             GuestPtrT<XSOCKADDR_IN> from_ptr, MappedU32 from_len,
                             GuestPtrT<XWSAOVERLAPPED> overlapped,
                             [[maybe_unused]] MappedVoid completion_routine) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!buffers || !InGuestRange(CurrentTitle().heap, buffers->buf_ptr, buffers->len)) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }
  u8* buf = CurrentTitle().membase.TranslateVirtual<u8*>(buffers->buf_ptr);
  const u32 buf_len = buffers->len;

  ReceiveResult result;
  if (!overlapped) {
    result.received = socket->RecvFrom(buf, buf_len, 0, &result.from, &result.from_len);
    if (result.received == -1) {
      SetCallError(*socket);
      return -1;
    }
  } else {
    result = socket->TryRecvFrom(buf, buf_len, 0);
    if (result.would_block) {
      const u32 event_handle = overlapped->event_handle;
      overlapped->internal_high = 0;
      overlapped->internal.Store(kWSAIOIncomplete, std::memory_order_release);
      if (event_handle) {
        if (auto event = CurrentTitle().objects.Get<kernel::IGuestEvent>(event_handle)) {
          event->Reset();
        }
      }
      auto complete = [overlapped, from_ptr, from_len, event_handle](const ReceiveResult& outcome) {
        CompleteReceive(overlapped, from_ptr, from_len, event_handle, outcome);
      };
      const bool started =
          socket->StartReceive(CurrentTitle().objects, CurrentTitle().objects.Get(socket_handle),
                               buf, buf_len, 0, std::move(complete));
      CurrentThread().SetLastError(started ? kWSAIOPending : kWSAEWOULDBLOCK);
      return -1;
    }
    if (result.received == -1) {
      CurrentThread().SetLastError(result.error);
      return -1;
    }
  }
  if (num_bytes_recv) {
    *num_bytes_recv = static_cast<u32>(result.received);
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  if (overlapped) {
    CompleteReceive(overlapped, from_ptr, from_len, overlapped->event_handle, result);
  } else {
    StoreFrom(from_ptr, from_len, result);
  }
  return 0;
}

/**
 * Cancels the pending overlapped receive of a socket, which then reports WSAECANCELLED.
 *
 * @param socket_handle The socket to cancel the receive on.
 * @return 0, or -1 with the last error set for a handle that is not a socket.
 */
u32 NetDll_WSACancelOverlappedIO_entry([[maybe_unused]] u32 caller, u32 socket_handle) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  socket->CancelReceive();
  return 0;
}

/**
 * Sends the buffers as one datagram.
 *
 * For a VDP socket, buffer 0 holds the title data length and buffer 1 the unencrypted title data.
 *
 * @param socket_handle The socket to send on.
 * @param buffers The buffers to send.
 * @param num_bytes_sent Receives the byte count.
 * @param flags The flags passed to the socket send.
 * @param to_ptr The destination address.
 * @param to_len The size of the destination address.
 * @param overlapped Optional overlapped block that receives the result, and whose event is set.
 * @param completion_routine Ignored.
 * @return 0 on success, -1 with the last error set on failure.
 */
u32 NetDll_WSASendTo_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                           GuestPtrT<XWSABUF> buffers, u32 num_buffers, MappedU32 num_bytes_sent,
                           u32 flags, GuestPtrT<XSOCKADDR_IN> to_ptr, u32 to_len,
                           GuestPtrT<XWSAOVERLAPPED> overlapped,
                           [[maybe_unused]] MappedVoid completion_routine) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  // The socket layer takes one buffer, so combine the guest buffers.
  std::vector<u8> combined_buffer_mem;
  u32 combined_buffer_size = 0;
  u32 combined_buffer_offset = 0;
  for (u32 i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    u8* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                CurrentTitle().membase.TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  const int sent =
      socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);
  const u32 error = sent == -1 ? CallError(*socket) : 0;
  const u32 bytes = sent == -1 ? 0 : static_cast<u32>(sent);
  if (num_bytes_sent) {
    *num_bytes_sent = bytes;
  }
  if (overlapped) {
    overlapped->internal_high = bytes;
    CurrentTitle().overlapped_dispatcher.Publish([&] {
      overlapped->internal.Store(error, std::memory_order_release);
      if (overlapped->event_handle) {
        SetGuestEvent(overlapped->event_handle);
      }
    });
  }
  if (sent == -1) {
    CurrentThread().SetLastError(error);
    return -1;
  }
  return 0;
}

/**
 * Reports the result of an overlapped operation.
 *
 * @param socket_handle Ignored.
 * @param overlapped The overlapped block of the operation.
 * @param bytes_transferred Receives the byte count.
 * @param wait Nonzero to wait on the event of a pending operation until it completes.
 * @param flags_ptr Receives 0.
 * @return 1 when the operation succeeded, 0 with its error set as the last error otherwise.
 */
u32 NetDll_WSAGetOverlappedResult_entry([[maybe_unused]] u32 caller,
                                        [[maybe_unused]] u32 socket_handle,
                                        GuestPtrT<XWSAOVERLAPPED> overlapped,
                                        MappedU32 bytes_transferred, u32 wait,
                                        MappedU32 flags_ptr) {
  auto load_status = [&overlapped] {
    return CurrentTitle().overlapped_dispatcher.Observe(
        [&] { return overlapped->internal.Load(std::memory_order_acquire); });
  };
  u32 status = load_status();
  while (status == kWSAIOIncomplete) {
    if (!wait || !overlapped->event_handle ||
        kernel::XFAILED(CurrentTitle().threads.GetObjectWaiter().WaitGuestObject(
            overlapped->event_handle, 0, 1, 0, nullptr))) {
      CurrentThread().SetLastError(kWSAIOIncomplete);
      return 0;
    }
    status = load_status();
  }
  if (bytes_transferred) {
    *bytes_transferred = overlapped->internal_high;
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  if (status) {
    CurrentThread().SetLastError(status);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, MappedU32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    // ERROR_INVALID_PARAMETER
    CurrentThread().SetLastError(87);
    return ~0u;
  }

  u64 timeout_wait = static_cast<u64>(-static_cast<i64>(timeout) * 10000);
  be_u32* guest_handles = events;
  const std::vector<kernel::X_HANDLE> handles(guest_handles, guest_handles + num_events);

  kernel::X_STATUS result = 0;
  do {
    result = CurrentTitle().threads.GetObjectWaiter().WaitGuestObjects(
        handles, wait_all, 6, 1, alertable, timeout != ~u32{0} ? &timeout_wait : nullptr);
  } while (result == kernel::X_STATUS_ALERTED);

  if (kernel::XFAILED(result)) {
    u32 error = kernel::xboxkrnl::RtlNtStatusToDosError(result);
    CurrentThread().SetLastError(error);
    return ~0u;
  }
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  auto added = CurrentTitle().objects.AddEvent(true, false);
  if (!added) {
    return 0;
  }
  return *added;
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  kernel::X_STATUS result = kernel::ToXStatus(CurrentTitle().objects.Close(event_handle));
  if (kernel::XFAILED(result)) {
    u32 error = kernel::xboxkrnl::RtlNtStatusToDosError(result);
    CurrentThread().SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  kernel::X_STATUS result = kernel::X_STATUS_SUCCESS;
  if (auto event = CurrentTitle().objects.Get<kernel::IGuestEvent>(event_handle)) {
    event->Reset();
  } else {
    result = kernel::X_STATUS_INVALID_HANDLE;
  }
  if (kernel::XFAILED(result)) {
    u32 error = kernel::xboxkrnl::RtlNtStatusToDosError(result);
    CurrentThread().SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  kernel::X_STATUS result = SetGuestEvent(event_handle);
  if (kernel::XFAILED(result)) {
    u32 error = kernel::xboxkrnl::RtlNtStatusToDosError(result);
    CurrentThread().SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  static const u32 kXNetGetXNAddrPending = 0x00000000;
  /** The network stack is uninitialized or no debugger was found. */
  static const u32 kXNetGetXNAddrNone = 0x00000001;
  /** The host has an Ethernet address and no IP address. */
  static const u32 kXNetGetXNAddrEthernet = 0x00000002;
  static const u32 kXNetGetXNAddrStatic = 0x00000004;
  static const u32 kXNetGetXNAddrDHCP = 0x00000008;
  static const u32 kXNetGetXNAddrPPPOE = 0x00000010;
  static const u32 kXNetGetXNAddrGateway = 0x00000020;
  static const u32 kXNetGetXNAddrDNS = 0x00000040;
  static const u32 kXNetGetXNAddrOnline = 0x00000080;
  static const u32 kXNetGetXNAddrTroubleshoot = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry([[maybe_unused]] u32 caller, GuestPtrT<XNADDR> addr_ptr) {
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // A zero abEnet makes titles that derive random numbers from it fail.
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::kXNetGetXNAddrStatic;
}

u32 NetDll_XNetGetDebugXnAddr_entry([[maybe_unused]] u32 caller, GuestPtrT<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  return XnAddrStatus::kXNetGetXNAddrNone;
}

u32 NetDll_XNetXnAddrToMachineId_entry([[maybe_unused]] u32 caller,
                                       [[maybe_unused]] GuestPtrT<XNADDR> addr_ptr,
                                       [[maybe_unused]] MappedU32 id_ptr) {
  // Non zero means the title is not signed in to Live.
  return 1;
}

void NetDll_XNetInAddrToString_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 in_addr,
                                     MappedString string_out, u32 string_size) {
  rex::string::CopyTruncating(string_out, "666.666.666.666", string_size);
}

u32 NetDll_XNetXnAddrToInAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] GuestPtrT<XNADDR> xn_addr,
                                    [[maybe_unused]] MappedVoid xid,
                                    [[maybe_unused]] MappedVoid in_addr) {
  return 1;
}

u32 NetDll_XNetInAddrToXnAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] MappedVoid in_addr,
                                    [[maybe_unused]] GuestPtrT<XNADDR> xn_addr,
                                    [[maybe_unused]] MappedVoid xid) {
  return 1;
}

u32 NetDll_XNetSetSystemLinkPort_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 port) {
  return 1;
}

struct XEthernetStatus {
  static const u32 kXNetEthernetLinkActive = 0x01;
  static const u32 kXNetEthernetLink100Mbps = 0x02;
  static const u32 kXNetEthernetLink10Mbps = 0x04;
  static const u32 kXNetEthernetLinkFullDuplex = 0x08;
  static const u32 kXNetEthernetLinkHalfDuplex = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry([[maybe_unused]] u32 caller) {
  return 0;
}

u32 NetDll_XNetDnsLookup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] MappedString host,
                               u32 event_handle, MappedU32 pdns) {
  if (pdns) {
    auto dns_guest = CurrentTitle().heap.SystemHeapAlloc(sizeof(XNDNS));
    auto dns = CurrentTitle().membase.TranslateVirtual<XNDNS*>(dns_guest);
    // A non zero status is an error.
    dns->status = 1;
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = CurrentTitle().objects.Get<kernel::IGuestEvent>(event_handle);
    REX_ASSERT(static_cast<bool>(ev));
    if (ev) {
      ev->Set();
    }
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry([[maybe_unused]] u32 caller, GuestPtrT<XNDNS> dns) {
  REX_XAM_REQUIRE_NONNULL(dns);
  CurrentTitle().heap.SystemHeapFree(dns.GetGuestAddress());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 flags,
                                      u32 event_handle, MappedU32 pqos) {
  // Titles read pqos even after a failure, so always set it.
  if (pqos) {
    auto qos_guest = CurrentTitle().heap.SystemHeapAlloc(sizeof(XNQOS));
    auto qos = CurrentTitle().membase.TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = CurrentTitle().objects.Get<kernel::IGuestEvent>(event_handle);
    REX_ASSERT(static_cast<bool>(ev));
    if (ev) {
      ev->Set();
    }
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry([[maybe_unused]] u32 caller, GuestPtrT<XNQOS> qos) {
  REX_XAM_REQUIRE_NONNULL(qos);
  CurrentTitle().heap.SystemHeapFree(qos.GetGuestAddress());
  return 0;
}

u32 NetDll_XNetQosListen_entry([[maybe_unused]] u32 caller, [[maybe_unused]] MappedVoid id,
                               [[maybe_unused]] MappedVoid data, [[maybe_unused]] u32 data_size,
                               [[maybe_unused]] u32 r7, [[maybe_unused]] u32 flags) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(MappedString addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  u32 addr = inet_addr(addr_ptr);
  // An empty string returns 0 instead of -1.
  if (addr == INADDR_NONE && addr_ptr.GetValue().empty()) {
    return 0;
  }

  return std::byteswap(addr);
}

u32 NetDll_socket_entry([[maybe_unused]] u32 caller, u32 af, u32 type, u32 protocol) {
  if (af > 0xFF) {
    // WSAEAFNOSUPPORT
    CurrentThread().SetLastError(0x273F);
    return -1;
  }
  if (type > 0xFF) {
    // WSAESOCKTNOSUPPORT
    CurrentThread().SetLastError(0x273C);
    return -1;
  }
  if (protocol > 0xFF) {
    // WSAEPROTONOSUPPORT
    CurrentThread().SetLastError(0x273B);
    return -1;
  }
  auto& table = CurrentTitle().objects;
  auto added = table.Emplace<SocketObject>(kernel::AddOptions{}, SocketAddressFamily(af),
                                           SocketType(type), SocketProtocol(protocol));
  if (!added) {
    CurrentThread().SetLastError(kernel::xboxkrnl::RtlNtStatusToDosError(kernel::ToXStatus(added)));
    return -1;
  }
  if (kernel::XFAILED((*table.Get<SocketObject>(*added)).Initialize())) {
    const u32 error = rex::platform::GetSocketError();
    kernel::CloseOwned(table, *added);
    CurrentThread().SetLastError(error);
    return -1;
  }

  return *added;
}

u32 NetDll_closesocket_entry([[maybe_unused]] u32 caller, u32 socket_handle) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  socket->CancelReceive();
  if (!socket->Cancel() || !CurrentTitle().objects.Close(socket_handle)) {
    CurrentThread().SetLastError(kWSAENOTSOCK);
    return -1;
  }
  return 0;
}

i32 NetDll_shutdown_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 how) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    SetCallError(*socket);
  }
  return ret;
}

u32 NetDll_setsockopt_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 level, u32 optname,
                            MappedVoid optval_ptr, u32 optlen) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  kernel::X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (kernel::XFAILED(status)) {
    CurrentThread().SetLastError(OptionError(*socket, status));
    return -1;
  }
  return 0;
}

u32 NetDll_getsockopt_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 level, u32 optname,
                            MappedVoid optval_ptr, MappedU32 optlen_ptr) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!optval_ptr || !optlen_ptr) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  u32 optlen = *optlen_ptr;
  kernel::X_STATUS status = socket->GetOption(level, optname, optval_ptr, &optlen);
  if (kernel::XFAILED(status)) {
    CurrentThread().SetLastError(OptionError(*socket, status));
    return -1;
  }
  *optlen_ptr = optlen;
  return 0;
}

u32 NetDll_ioctlsocket_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 cmd,
                             MappedVoid arg_ptr) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  kernel::X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (kernel::XFAILED(status)) {
    SetCallError(*socket);
    return -1;
  }

  return 0;
}

u32 NetDll_bind_entry([[maybe_unused]] u32 caller, u32 socket_handle, GuestPtrT<XSOCKADDR_IN> name,
                      u32 namelen) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  kernel::X_STATUS status = socket->Bind(&native_name, namelen);
  if (kernel::XFAILED(status)) {
    CurrentThread().SetLastError(IsValidNameLength(namelen) ? CallError(*socket) : kWSAEFAULT);
    return -1;
  }

  return 0;
}

u32 NetDll_getsockname_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                             GuestPtrT<XSOCKADDR> name_ptr, MappedU32 namelen_ptr) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!name_ptr || !namelen_ptr || *namelen_ptr < sizeof(XSOCKADDR_IN)) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  N_XSOCKADDR native_name;
  kernel::X_STATUS status = socket->GetName(&native_name);
  if (kernel::XFAILED(status)) {
    CurrentThread().SetLastError(
        status == kernel::X_STATUS_INVALID_PARAMETER_1 ? kWSAEINVAL : CallError(*socket));
    return -1;
  }
  name_ptr->address_family = native_name.address_family;
  std::memcpy(name_ptr->sa_data, native_name.sa_data, sizeof(native_name.sa_data));
  *namelen_ptr = sizeof(XSOCKADDR_IN);
  return 0;
}

u32 NetDll_connect_entry([[maybe_unused]] u32 caller, u32 socket_handle, GuestPtrT<XSOCKADDR> name,
                         u32 namelen) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  N_XSOCKADDR native_name(name);
  kernel::X_STATUS status = socket->Connect(&native_name, namelen);
  if (kernel::XFAILED(status)) {
    CurrentThread().SetLastError(IsValidNameLength(namelen) ? CallError(*socket) : kWSAEFAULT);
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  kernel::X_STATUS status = socket->Listen(backlog);
  if (kernel::XFAILED(status)) {
    SetCallError(*socket);
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                        GuestPtrT<XSOCKADDR> addr_ptr, MappedU32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    CurrentThread().SetLastError(0x271E);
    return -1;
  }

  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = static_cast<int>(std::min<u32>(*addrlen_ptr, sizeof(N_XSOCKADDR)));
  kernel::X_HANDLE new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket != kernel::kXInvalidHandleValue) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data,
                static_cast<size_t>(
                    std::clamp(native_len - 2, 0, static_cast<int>(sizeof(native_addr.sa_data)))));
    *addrlen_ptr = native_len;

    return new_socket;
  } else {
    SetCallError(*socket);
    return -1;
  }
}

struct x_fd_set {
  be_u32 fd_count;
};

std::span<be_u32> GetEntries(GuestPtrT<x_fd_set> set) {
  return {
      CurrentTitle().membase.TranslateVirtual<be_u32*>(set.GetGuestAddress() + sizeof(x_fd_set)),
      set->fd_count};
}

struct HostSet {
  std::vector<kernel::Pinned<SocketObject>> sockets;

  bool Load(GuestPtrT<x_fd_set> guest_set) {
    if (!guest_set) {
      return true;
    }
    for (const u32 socket_handle : GetEntries(guest_set)) {
      auto socket = CurrentTitle().objects.Get<SocketObject>(socket_handle);
      if (!socket || socket->closed) {
        return false;
      }
      sockets.push_back(std::move(socket));
    }
    return true;
  }

  void Watch(std::vector<rex::platform::SocketWait>& waits,
             rex::platform::SocketReady ready) const {
    for (const kernel::Pinned<SocketObject>& socket : sockets) {
      waits.push_back({socket->socket.GetHandle(), socket->event.get(), ready});
    }
  }

  void Store(GuestPtrT<x_fd_set> guest_set,
             std::span<const rex::platform::SocketReady> ready) const {
    if (!guest_set) {
      return;
    }
    const std::span<be_u32> entries = GetEntries(guest_set);
    u32 count = 0;
    for (size_t i = 0; i < sockets.size(); ++i) {
      if (ready[i] != rex::platform::SocketReady::kNone) {
        entries[count++] = sockets[i].GetHandle();
      }
    }
    guest_set->fd_count = count;
  }
};

i32 NetDll_select_entry([[maybe_unused]] i32 caller, [[maybe_unused]] i32 nfds,
                        GuestPtrT<x_fd_set> readfds, GuestPtrT<x_fd_set> writefds,
                        GuestPtrT<x_fd_set> exceptfds, MappedVoid timeout_ptr) {
  HostSet host_readfds;
  HostSet host_writefds;
  HostSet host_exceptfds;
  if (!host_readfds.Load(readfds) || !host_writefds.Load(writefds) ||
      !host_exceptfds.Load(exceptfds)) {
    CurrentThread().SetLastError(kWSAENOTSOCK);
    return -1;
  }
  std::vector<rex::platform::SocketWait> waits;
  host_readfds.Watch(waits, rex::platform::SocketReady::kRead);
  host_writefds.Watch(waits, rex::platform::SocketReady::kWrite);
  host_exceptfds.Watch(waits, rex::platform::SocketReady::kExcept);
  if (waits.empty()) {
    CurrentThread().SetLastError(kWSAEINVAL);
    return -1;
  }

  auto wait_time = std::chrono::milliseconds::max();
  if (timeout_ptr) {
    i32 timeout_sec = static_cast<i32>(timeout_ptr.AsArray<i32>()[0]);
    i32 timeout_usec = static_cast<i32>(timeout_ptr.AsArray<i32>()[1]);
    Clock::ScaleGuestDurationTimeval(&timeout_sec, &timeout_usec);
    wait_time = std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::seconds(timeout_sec) + std::chrono::microseconds(timeout_usec));
  }

  auto closed = [&] {
    bool any = false;
    for (const HostSet* set : {&host_readfds, &host_writefds, &host_exceptfds}) {
      for (const kernel::Pinned<SocketObject>& socket : set->sockets) {
        if (socket->closed) {
          if (socket->event) {
            socket->event->Set();
          }
          any = true;
        }
      }
    }
    if (any) {
      CurrentThread().SetLastError(kWSAEINTR);
    }
    return any;
  };

  std::vector<rex::platform::SocketReady> ready(waits.size());
  using clock = std::chrono::steady_clock;
  const bool infinite = wait_time == std::chrono::milliseconds::max();
  const auto deadline = infinite ? clock::time_point::max() : clock::now() + wait_time;
  int ret = 0;
  while (true) {
    ret = rex::platform::PollSockets(waits, ready);
    if (ret == -1) {
      CurrentThread().SetLastError(rex::platform::GetSocketError());
      return -1;
    }
    if (ret != 0 || (!infinite && clock::now() >= deadline)) {
      break;
    }
    if (closed()) {
      return -1;
    }
    const auto left = infinite
                          ? std::chrono::milliseconds::max()
                          : std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
    if (CurrentTitle().threads.GetObjectWaiter().WaitHostSockets(waits, left) ==
        rex::platform::WaitResult::kFailed) {
      CurrentThread().SetLastError(rex::platform::GetSocketError());
      return -1;
    }
    if (closed()) {
      return -1;
    }
  }

  std::span<const rex::platform::SocketReady> results = ready;
  for (auto [set, guest_set] :
       {std::pair{&host_readfds, readfds}, std::pair{&host_writefds, writefds},
        std::pair{&host_exceptfds, exceptfds}}) {
    set->Store(guest_set, results.first(set->sockets.size()));
    results = results.subspan(set->sockets.size());
  }
  return ret;
}

u32 NetDll_recv_entry([[maybe_unused]] u32 caller, u32 socket_handle, MappedVoid buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  const int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret == -1) {
    SetCallError(*socket);
  }
  return ret;
}

u32 NetDll_recvfrom_entry([[maybe_unused]] u32 caller, u32 socket_handle, MappedVoid buf_ptr,
                          u32 buf_len, u32 flags, GuestPtrT<XSOCKADDR_IN> from_ptr,
                          MappedU32 fromlen_ptr) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  u32 native_fromlen = fromlen_ptr ? fromlen_ptr.GetValue() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    SetCallError(*socket);
  }

  return ret;
}

u32 NetDll_send_entry([[maybe_unused]] u32 caller, u32 socket_handle, MappedVoid buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  const int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret == -1) {
    SetCallError(*socket);
  }
  return ret;
}

u32 NetDll_sendto_entry([[maybe_unused]] u32 caller, u32 socket_handle, MappedVoid buf_ptr,
                        u32 buf_len, u32 flags, GuestPtrT<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = GetSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    CurrentThread().SetLastError(kWSAEFAULT);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  const int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  if (ret == -1) {
    SetCallError(*socket);
  }
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, GuestPtrT<x_fd_set> fd_set) {
  return std::ranges::any_of(GetEntries(fd_set),
                             [socket_handle](u32 entry) { return entry == socket_handle; })
             ? 1
             : 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  CurrentThread().SetLastError(error_code);
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__NetDll_XNetStartup, rex::xam::NetDll_XNetStartup_entry)
REX_XAM_BODY(__imp__NetDll_XNetCleanup, rex::xam::NetDll_XNetCleanup_entry)
REX_XAM_BODY(__imp__NetDll_XNetGetOpt, rex::xam::NetDll_XNetGetOpt_entry)
REX_XAM_BODY(__imp__NetDll_XNetRandom, rex::xam::NetDll_XNetRandom_entry)
REX_XAM_BODY(__imp__NetDll_WSAStartup, rex::xam::NetDll_WSAStartup_entry)
REX_XAM_BODY(__imp__NetDll_WSACleanup, rex::xam::NetDll_WSACleanup_entry)
REX_XAM_BODY(__imp__NetDll_WSAGetLastError, rex::xam::NetDll_WSAGetLastError_entry)
REX_XAM_BODY(__imp__NetDll_WSARecvFrom, rex::xam::NetDll_WSARecvFrom_entry)
REX_XAM_BODY(__imp__NetDll_WSASendTo, rex::xam::NetDll_WSASendTo_entry)
REX_XAM_BODY(__imp__NetDll_WSAGetOverlappedResult, rex::xam::NetDll_WSAGetOverlappedResult_entry)
REX_XAM_BODY(__imp__NetDll_WSAWaitForMultipleEvents,
             rex::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_XAM_BODY(__imp__NetDll_WSACreateEvent, rex::xam::NetDll_WSACreateEvent_entry)
REX_XAM_BODY(__imp__NetDll_WSACloseEvent, rex::xam::NetDll_WSACloseEvent_entry)
REX_XAM_BODY(__imp__NetDll_WSAResetEvent, rex::xam::NetDll_WSAResetEvent_entry)
REX_XAM_BODY(__imp__NetDll_WSASetEvent, rex::xam::NetDll_WSASetEvent_entry)
REX_XAM_BODY(__imp__NetDll_XNetGetTitleXnAddr, rex::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_XAM_BODY(__imp__NetDll_XNetGetDebugXnAddr, rex::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_XAM_BODY(__imp__NetDll_XNetXnAddrToMachineId, rex::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_XAM_BODY(__imp__NetDll_XNetInAddrToString, rex::xam::NetDll_XNetInAddrToString_entry)
REX_XAM_BODY(__imp__NetDll_XNetXnAddrToInAddr, rex::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_XAM_BODY(__imp__NetDll_XNetInAddrToXnAddr, rex::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_XAM_BODY(__imp__NetDll_XNetSetSystemLinkPort, rex::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_XAM_BODY(__imp__NetDll_XNetGetEthernetLinkStatus,
             rex::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_XAM_BODY(__imp__NetDll_XNetDnsLookup, rex::xam::NetDll_XNetDnsLookup_entry)
REX_XAM_BODY(__imp__NetDll_XNetDnsRelease, rex::xam::NetDll_XNetDnsRelease_entry)
REX_XAM_BODY(__imp__NetDll_XNetQosServiceLookup, rex::xam::NetDll_XNetQosServiceLookup_entry)
REX_XAM_BODY(__imp__NetDll_XNetQosRelease, rex::xam::NetDll_XNetQosRelease_entry)
REX_XAM_BODY(__imp__NetDll_XNetQosListen, rex::xam::NetDll_XNetQosListen_entry)
REX_XAM_BODY(__imp__NetDll_inet_addr, rex::xam::NetDll_inet_addr_entry)
REX_XAM_BODY(__imp__NetDll_socket, rex::xam::NetDll_socket_entry)
REX_XAM_BODY(__imp__NetDll_closesocket, rex::xam::NetDll_closesocket_entry)
REX_XAM_BODY(__imp__NetDll_shutdown, rex::xam::NetDll_shutdown_entry)
REX_XAM_BODY(__imp__NetDll_setsockopt, rex::xam::NetDll_setsockopt_entry)
REX_XAM_BODY(__imp__NetDll_ioctlsocket, rex::xam::NetDll_ioctlsocket_entry)
REX_XAM_BODY(__imp__NetDll_bind, rex::xam::NetDll_bind_entry)
REX_XAM_BODY(__imp__NetDll_connect, rex::xam::NetDll_connect_entry)
REX_XAM_BODY(__imp__NetDll_getsockname, rex::xam::NetDll_getsockname_entry)
REX_XAM_BODY(__imp__NetDll_getsockopt, rex::xam::NetDll_getsockopt_entry)
REX_XAM_BODY(__imp__NetDll_listen, rex::xam::NetDll_listen_entry)
REX_XAM_BODY(__imp__NetDll_accept, rex::xam::NetDll_accept_entry)
REX_XAM_BODY(__imp__NetDll_select, rex::xam::NetDll_select_entry)
REX_XAM_BODY(__imp__NetDll_recv, rex::xam::NetDll_recv_entry)
REX_XAM_BODY(__imp__NetDll_recvfrom, rex::xam::NetDll_recvfrom_entry)
REX_XAM_BODY(__imp__NetDll_send, rex::xam::NetDll_send_entry)
REX_XAM_BODY(__imp__NetDll_sendto, rex::xam::NetDll_sendto_entry)
REX_XAM_BODY(__imp__NetDll___WSAFDIsSet, rex::xam::NetDll___WSAFDIsSet_entry)
REX_XAM_BODY(__imp__NetDll_WSASetLastError, rex::xam::NetDll_WSASetLastError_entry)

REX_XAM_BODY(__imp__NetDll_WSACancelOverlappedIO, rex::xam::NetDll_WSACancelOverlappedIO_entry)
