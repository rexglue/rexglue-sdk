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

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/event_object.h>
#include <rex/runtime/socket_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"
#include "runtime/thread_wait.h"

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
namespace kernel {
namespace xam {

constexpr u32 kWSAEFAULT = 0x271E;
constexpr u32 kMaxSocketLength = static_cast<u32>(std::numeric_limits<i32>::max());
using namespace rex::runtime;

enum : u8 {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

struct XNADDR {
  // IN_ADDR fields are in network byte order.
  in_addr ina;       /**< IP address, zero unless static or DHCP. */
  in_addr inaOnline; /**< Online IP address, zero when not online. */
  be_u16 wPortOnline;
  u8 abEnet[6]; /**< Ethernet MAC address. */
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
  be_u16 probes_xmit;
  be_u16 probes_recv;
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

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    REX_ASSERT(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  return 0;
}

u32 NetDll_XNetCleanup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] mapped_void params) {
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  REX_ASSERT(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        // WSAEMSGSIZE
        return 0x2738;
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REX_KERNEL_STUB("NetDll_XNetGetOpt: option {}", option_id);
      // WSAEINVAL
      return 0x2726;
  }
}

u32 NetDll_XNetRandom_entry([[maybe_unused]] u32 caller, mapped_void buffer_ptr, u32 length) {
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry([[maybe_unused]] u32 caller, u16 version,
                            ppc_ptr_t<X_WSADATA> data_ptr) {
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;
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
  return KernelThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 socket,
                             [[maybe_unused]] ppc_ptr_t<XWSABUF> buffers_ptr,
                             [[maybe_unused]] u32 buffer_count,
                             [[maybe_unused]] mapped_u32 num_bytes_recv,
                             [[maybe_unused]] mapped_u32 flags_ptr,
                             [[maybe_unused]] ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             [[maybe_unused]] ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             [[maybe_unused]] mapped_void completion_routine_ptr) {
  // Return an error so the title does not wait for packets.
  return -1;
}

/**
 * Sends the buffers as 1 datagram.
 *
 * For a VDP socket, buffer 0 holds the title data length and buffer 1 the unencrypted title data.
 *
 * @param caller The caller type.
 * @param socket_handle The socket handle.
 * @param buffers The buffers to send.
 * @param num_buffers The count of buffers.
 * @param num_bytes_sent Unused.
 * @param flags The send flags.
 * @param to_ptr The destination address.
 * @param to_len The size of the destination address.
 * @param overlapped Must be null.
 * @param completion_routine Must be null.
 * @return 0 on success, -1 when the handle is not a socket.
 */
u32 NetDll_WSASendTo_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                           ppc_ptr_t<XWSABUF> buffers, u32 num_buffers,
                           [[maybe_unused]] mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  // The socket layer takes 1 buffer, so combine the guest buffers.
  std::vector<u8> combined_buffer_mem;
  u32 combined_buffer_size = 0;
  u32 combined_buffer_offset = 0;
  for (u32 i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    u8* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                runtime::Current().memory()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    // ERROR_INVALID_PARAMETER
    KernelThread::SetLastError(87);
    return ~0u;
  }

  u64 timeout_wait = static_cast<u64>(-static_cast<i64>(timeout) * 10000);

  X_STATUS result = 0;
  do {
    result = xboxkrnl::NtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                  timeout != ~u32{0} ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    u32 error = xboxkrnl::RtlNtStatusToDosError(result);
    KernelThread::SetLastError(error);
    return ~0u;
  }
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  auto added =
      runtime::Current().object_table().CreateObject<EventObject>(0, sizeof(X_KEVENT), true, false);
  if (!added) {
    return 0;
  }
  return *added;
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = ToXStatus(runtime::Current().object_table().Close(event_handle));
  if (XFAILED(result)) {
    u32 error = xboxkrnl::RtlNtStatusToDosError(result);
    KernelThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::NtClearEvent(event_handle);
  if (XFAILED(result)) {
    u32 error = xboxkrnl::RtlNtStatusToDosError(result);
    KernelThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::NtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    u32 error = xboxkrnl::RtlNtStatusToDosError(result);
    KernelThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  static const u32 XNET_GET_XNADDR_PENDING = 0x00000000;
  /** The network stack is uninitialized or no debugger was found. */
  static const u32 XNET_GET_XNADDR_NONE = 0x00000001;
  /** The host has an Ethernet address and no IP address. */
  static const u32 XNET_GET_XNADDR_ETHERNET = 0x00000002;
  static const u32 XNET_GET_XNADDR_STATIC = 0x00000004;
  static const u32 XNET_GET_XNADDR_DHCP = 0x00000008;
  static const u32 XNET_GET_XNADDR_PPPOE = 0x00000010;
  static const u32 XNET_GET_XNADDR_GATEWAY = 0x00000020;
  static const u32 XNET_GET_XNADDR_DNS = 0x00000040;
  static const u32 XNET_GET_XNADDR_ONLINE = 0x00000080;
  static const u32 XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // A zero abEnet makes titles that derive random numbers from it fail.
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry([[maybe_unused]] u32 caller,
                                       [[maybe_unused]] ppc_ptr_t<XNADDR> addr_ptr,
                                       [[maybe_unused]] mapped_u32 id_ptr) {
  // Non zero means the title is not signed in to Live.
  return 1;
}

void NetDll_XNetInAddrToString_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 in_addr,
                                     mapped_string string_out, u32 string_size) {
  rex::copy_truncating(string_out, "666.666.666.666", string_size);
}

/**
 * Stub for converting an XNet address to an IN_ADDR, which later socket calls use as a handle
 * to the address.
 *
 * @param caller The caller type.
 * @param xn_addr The XNet address.
 * @param xid The key identifier.
 * @param in_addr Receives the IN_ADDR.
 * @return 1.
 */
u32 NetDll_XNetXnAddrToInAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] ppc_ptr_t<XNADDR> xn_addr,
                                    [[maybe_unused]] mapped_void xid,
                                    [[maybe_unused]] mapped_void in_addr) {
  return 1;
}

/**
 * Stub for the reverse conversion of NetDll_XNetXnAddrToInAddr_entry.
 *
 * @param caller The caller type.
 * @param in_addr The IN_ADDR.
 * @param xn_addr Receives the XNet address.
 * @param xid The key identifier.
 * @return 1.
 */
u32 NetDll_XNetInAddrToXnAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] mapped_void in_addr,
                                    [[maybe_unused]] ppc_ptr_t<XNADDR> xn_addr,
                                    [[maybe_unused]] mapped_void xid) {
  return 1;
}

/**
 * Stub that reserves a port for system link.
 *
 * @param caller The caller type.
 * @param port The port to reserve.
 * @return 1.
 */
u32 NetDll_XNetSetSystemLinkPort_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 port) {
  return 1;
}

struct XEthernetStatus {
  static const u32 XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const u32 XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const u32 XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const u32 XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const u32 XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry([[maybe_unused]] u32 caller) {
  return 0;
}

u32 NetDll_XNetDnsLookup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] mapped_string host,
                               u32 event_handle, mapped_u32 pdns) {
  if (pdns) {
    auto dns_guest = runtime::Current().memory()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = runtime::Current().memory()->TranslateVirtual<XNDNS*>(dns_guest);
    // A non zero status is an error.
    dns->status = 1;
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = runtime::Current().object_table().Get<EventObject>(event_handle);
    REX_ASSERT(static_cast<bool>(ev));
    if (ev) {
      ev->Set();
    }
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNDNS> dns) {
  REX_KERNEL_ARG(dns);
  runtime::Current().memory()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 flags,
                                      u32 event_handle, mapped_u32 pqos) {
  // Titles read pqos even after a failure, so always set it.
  if (pqos) {
    auto qos_guest = runtime::Current().memory()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = runtime::Current().memory()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = runtime::Current().object_table().Get<EventObject>(event_handle);
    REX_ASSERT(static_cast<bool>(ev));
    if (ev) {
      ev->Set();
    }
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNQOS> qos) {
  REX_KERNEL_ARG(qos);
  runtime::Current().memory()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry([[maybe_unused]] u32 caller, [[maybe_unused]] mapped_void id,
                               [[maybe_unused]] mapped_void data, [[maybe_unused]] u32 data_size,
                               [[maybe_unused]] u32 r7, [[maybe_unused]] u32 flags) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  u32 addr = inet_addr(addr_ptr);
  // An empty string returns 0 instead of -1.
  if (addr == INADDR_NONE && addr_ptr.value().empty()) {
    return 0;
  }

  return std::byteswap(addr);
}

u32 NetDll_socket_entry([[maybe_unused]] u32 caller, u32 af, u32 type, u32 protocol) {
  if (af > 0xFF) {
    // WSAEAFNOSUPPORT
    KernelThread::SetLastError(0x273F);
    return -1;
  }
  if (type > 0xFF) {
    // WSAESOCKTNOSUPPORT
    KernelThread::SetLastError(0x273C);
    return -1;
  }
  if (protocol > 0xFF) {
    // WSAEPROTONOSUPPORT
    KernelThread::SetLastError(0x273B);
    return -1;
  }
  auto& table = runtime::Current().object_table();
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  auto added = table.Emplace<SocketObject>(AddOptions{}, SocketAddressFamily(af), SocketType(type),
                                           SocketProtocol(protocol));
  if (added) {
    handle = *added;
  }
  X_STATUS result = ToXStatus(added);
  if (XSUCCEEDED(result)) {
    result = (*table.Get<SocketObject>(handle)).Initialize();
    if (XFAILED(result)) {
      runtime::CloseOwned(table, handle);
    }
  }

  if (XFAILED(result)) {
    u32 error = xboxkrnl::RtlNtStatusToDosError(result);
    KernelThread::SetLastError(error);
    return -1;
  }

  return handle;
}

u32 NetDll_closesocket_entry([[maybe_unused]] u32 caller, u32 socket_handle) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  socket->Close();
  if (const auto closed = runtime::Current().object_table().Close(socket_handle); !closed) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }
  return 0;
}

i32 NetDll_shutdown_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 how) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    u32 error_code = WSAGetLastError();
    KernelThread::SetLastError(error_code);
#else
    KernelThread::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 cmd,
                             mapped_void arg_ptr) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    KernelThread::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_bind_entry([[maybe_unused]] u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                      u32 namelen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    KernelThread::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry([[maybe_unused]] u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name,
                         u32 namelen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    KernelThread::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    KernelThread::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                        ppc_ptr_t<XSOCKADDR> addr_ptr, mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    KernelThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = static_cast<int>(std::min<u32>(*addrlen_ptr, sizeof(N_XSOCKADDR)));
  X_HANDLE new_socket = socket->Accept(runtime::Current(), &native_addr, &native_len);
  if (new_socket != X_INVALID_HANDLE_VALUE) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data,
                static_cast<size_t>(
                    std::clamp(native_len - 2, 0, static_cast<int>(sizeof(native_addr.sa_data)))));
    *addrlen_ptr = native_len;

    return new_socket;
  } else {
    return -1;
  }
}

struct x_fd_set {
  be_u32 fd_count;
  be_u32 fd_array[64];
};

struct host_set {
  u32 count;
  Pinned<SocketObject> sockets[64];

  void Load(const x_fd_set* guest_set) {
    REX_ASSERT(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (u32 i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == X_INVALID_SOCKET) {
        this->count = i;
        break;
      }
      auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = std::move(socket);
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (u32 i = 0; i < this->count; ++i) {
      guest_set->fd_array[guest_set->fd_count++] = this->sockets[i].handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (u32 i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle, native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    u32 new_count = 0;
    for (u32 i = 0; i < this->count; ++i) {
      if (FD_ISSET(this->sockets[i]->native_handle, native_set)) {
        if (new_count != i) {
          this->sockets[new_count] = std::move(this->sockets[i]);
        }
        new_count++;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry([[maybe_unused]] i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  host_set host_writefds = {};
  host_set host_exceptfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
  }
  if (writefds) {
    host_writefds.Load(writefds);
  }
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
  }
  std::vector<rex::platform::SocketWait> waits;
  auto watch = [&waits](const host_set& set, rex::platform::SocketReady ready) {
    for (u32 i = 0; i < set.count; ++i) {
      waits.push_back({static_cast<rex::platform::SocketHandle>(set.sockets[i]->native_handle),
                       set.sockets[i]->event.get(), ready});
    }
  };
  watch(host_readfds, rex::platform::SocketReady::kRead);
  watch(host_writefds, rex::platform::SocketReady::kWrite);
  watch(host_exceptfds, rex::platform::SocketReady::kExcept);

  timeval* timeout_in = nullptr;
  timeval timeout = {};
  auto wait_time = std::chrono::milliseconds::max();
  if (timeout_ptr) {
    i32 timeout_sec = static_cast<i32>(timeout_ptr.as_array<i32>()[0]);
    i32 timeout_usec = static_cast<i32>(timeout_ptr.as_array<i32>()[1]);
    Clock::ScaleGuestDurationTimeval(&timeout_sec, &timeout_usec);
    timeout = {timeout_sec, timeout_usec};
    timeout_in = &timeout;
    wait_time = std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::seconds(timeout_sec) + std::chrono::microseconds(timeout_usec));
  }

  fd_set native_readfds = {};
  fd_set native_writefds = {};
  fd_set native_exceptfds = {};
  auto select_now = [&](timeval* limit) {
    host_readfds.Store(&native_readfds);
    host_writefds.Store(&native_writefds);
    host_exceptfds.Store(&native_exceptfds);
    return select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                  exceptfds ? &native_exceptfds : nullptr, limit);
  };

  int ret = 0;
  if (waits.empty()) {
    ret = select_now(timeout_in);
  } else {
    using clock = std::chrono::steady_clock;
    const bool infinite = wait_time == std::chrono::milliseconds::max();
    const auto deadline = infinite ? clock::time_point::max() : clock::now() + wait_time;
    while (true) {
      timeval zero = {};
      ret = select_now(&zero);
      if (ret != 0 || (!infinite && clock::now() >= deadline)) {
        break;
      }
      const auto left = infinite
                            ? std::chrono::milliseconds::max()
                            : std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now());
      if (runtime::WaitHostSockets(waits, left) == rex::platform::WaitResult::kFailed) {
        ret = -1;
        break;
      }
    }
  }

  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  return ret;
}

u32 NetDll_recv_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    KernelThread::SetLastError(kWSAEFAULT);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

u32 NetDll_recvfrom_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                          u32 buf_len, u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr,
                          mapped_u32 fromlen_ptr) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    KernelThread::SetLastError(kWSAEFAULT);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  u32 native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
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
#if REX_PLATFORM_WIN32
    u32 error_code = WSAGetLastError();
    KernelThread::SetLastError(error_code);
#else
    KernelThread::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    KernelThread::SetLastError(kWSAEFAULT);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                        u32 buf_len, u32 flags, ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    KernelThread::SetLastError(0x2736);
    return -1;
  }

  if (buf_len > kMaxSocketLength) {
    KernelThread::SetLastError(kWSAEFAULT);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const u8 max_fd_count = std::min(static_cast<u32>(fd_set->fd_count), static_cast<u32>(64));
  for (u8 i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  KernelThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetConnect);
REX_EXPORT_STUB(__imp__NetDll_XNetCreateKey);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetConnectStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT_STUB(__imp__NetDll_XNetQosLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetRegisterKey);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterKey);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT_STUB(__imp__NetDll_getpeername);
REX_EXPORT_STUB(__imp__NetDll_getsockname);
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
