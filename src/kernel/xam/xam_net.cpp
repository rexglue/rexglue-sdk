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

#include <bit>
#include <cstring>

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
#include <rex/runtime/xevent.h>
#include <rex/runtime/xsocket.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"
#include "runtime/thread_wait.h"

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum : u8 {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
struct XNADDR {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;           // IP address (zero if not static/DHCP)
  in_addr inaOnline;     // Online IP address (zero if not online)
  be_u16 wPortOnline;    // Online port
  uint8_t abEnet[6];     // Ethernet MAC address
  uint8_t abOnline[20];  // Online identification
};

struct XNDNS {
  be_i32 status;
  be_u32 cina;
  in_addr aina[8];
};

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
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
    } offset;  // must be named to avoid GCC error
    be_u32 pointer;
  };
  be_u32 event_handle;
};

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    REX_ASSERT(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(runtime::Current());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] mapped_void params) {
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  REX_ASSERT(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REX_KERNEL_STUB("NetDll_XNetGetOpt: option {}", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry([[maybe_unused]] u32 caller, mapped_void buffer_ptr, u32 length) {
  // Constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry([[maybe_unused]] u32 caller, u16 version,
                            ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
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
    // Guess these values!
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
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return ThreadRecord::GetLastError();
}

u32 NetDll_WSARecvFrom_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 socket,
                             [[maybe_unused]] ppc_ptr_t<XWSABUF> buffers_ptr,
                             [[maybe_unused]] u32 buffer_count,
                             [[maybe_unused]] mapped_u32 num_bytes_recv,
                             [[maybe_unused]] mapped_u32 flags_ptr,
                             [[maybe_unused]] ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             [[maybe_unused]] ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             [[maybe_unused]] mapped_void completion_routine_ptr) {
  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
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
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                runtime::Current().memory()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  SocketSendTo(*socket, combined_buffer_mem.data(), combined_buffer_size, flags, &native_to,
               to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    ThreadRecord::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  u64 timeout_wait = static_cast<u64>(-static_cast<i64>(timeout) * 10000);

  X_STATUS result = 0;
  do {
    result = xboxkrnl::NtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                  timeout != ~u32{0} ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::RtlNtStatusToDosError(result);
    ThreadRecord::SetLastError(error);
    return ~0u;
  }
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  auto added = runtime::Current().object_table().CreateWithBlock<EventObject>(0, sizeof(X_KEVENT),
                                                                              true, false);
  if (!added) {
    return 0;
  }
  return *added;
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = ToXStatus(runtime::Current().object_table().Close(event_handle));
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::RtlNtStatusToDosError(result);
    ThreadRecord::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::NtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::RtlNtStatusToDosError(result);
    ThreadRecord::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::NtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::RtlNtStatusToDosError(result);
    ThreadRecord::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // Return a loopback address.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's console version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The console specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry([[maybe_unused]] u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry([[maybe_unused]] u32 caller,
                                       [[maybe_unused]] ppc_ptr_t<XNADDR> addr_ptr,
                                       [[maybe_unused]] mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 in_addr,
                                     mapped_string string_out, u32 string_size) {
  rex::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] ppc_ptr_t<XNADDR> xn_addr,
                                    [[maybe_unused]] mapped_void xid,
                                    [[maybe_unused]] mapped_void in_addr) {
  return 1;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry([[maybe_unused]] u32 caller,
                                    [[maybe_unused]] mapped_void in_addr,
                                    [[maybe_unused]] ppc_ptr_t<XNADDR> xn_addr,
                                    [[maybe_unused]] mapped_void xid) {
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry([[maybe_unused]] u32 caller, [[maybe_unused]] u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry([[maybe_unused]] u32 caller) {
  return 0;
}

u32 NetDll_XNetDnsLookup_entry([[maybe_unused]] u32 caller, [[maybe_unused]] mapped_string host,
                               u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): implement this
  if (pdns) {
    auto dns_guest = runtime::Current().memory()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = runtime::Current().memory()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = runtime::Current().object_table().Get<EventObject>(event_handle);
    REX_ASSERT(static_cast<bool>(ev));
    if (ev) {
      EventSet(*ev);
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
  // Set pqos as some games will try accessing it despite non-successful result
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
      EventSet(*ev);
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

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == INADDR_NONE && addr_ptr.value().empty()) {
    return 0;
  }

  return std::byteswap(addr);
}

u32 NetDll_socket_entry([[maybe_unused]] u32 caller, u32 af, u32 type, u32 protocol) {
  auto& table = runtime::Current().object_table();
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  auto added = table.Emplace<SocketObject>(
      AddOptions{}, SocketAddressFamily(static_cast<uint32_t>(af)),
      SocketType(static_cast<uint32_t>(type)), SocketProtocol(static_cast<uint32_t>(protocol)));
  if (added) {
    handle = *added;
  }
  X_STATUS result = ToXStatus(added);
  if (XSUCCEEDED(result)) {
    result = SocketInitialize(*table.Get<SocketObject>(handle));
    if (XFAILED(result)) {
      (void)table.Close(handle);
    }
  }

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::RtlNtStatusToDosError(result);
    ThreadRecord::SetLastError(error);
    return -1;
  }

  return handle;
}

u32 NetDll_closesocket_entry([[maybe_unused]] u32 caller, u32 socket_handle) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  SocketClose(*socket);
  (void)runtime::Current().object_table().Close(socket_handle);
  return 0;
}

i32 NetDll_shutdown_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 how) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  auto ret = SocketShutdown(*socket, how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    ThreadRecord::SetLastError(error_code);
#else
    ThreadRecord::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = SocketSetOption(*socket, level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry([[maybe_unused]] u32 caller, u32 socket_handle, u32 cmd,
                             mapped_void arg_ptr) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = SocketIOControl(*socket, cmd, arg_ptr);
  if (XFAILED(status)) {
    ThreadRecord::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry([[maybe_unused]] u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                      u32 namelen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = SocketBind(*socket, &native_name, namelen);
  if (XFAILED(status)) {
    ThreadRecord::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry([[maybe_unused]] u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name,
                         u32 namelen) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = SocketConnect(*socket, &native_name, namelen);
  if (XFAILED(status)) {
    ThreadRecord::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry([[maybe_unused]] u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = SocketListen(*socket, backlog);
  if (XFAILED(status)) {
    ThreadRecord::SetLastError(xboxkrnl::RtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry([[maybe_unused]] u32 caller, u32 socket_handle,
                        ppc_ptr_t<XSOCKADDR> addr_ptr, mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    ThreadRecord::SetLastError(0x271E);
    return -1;
  }

  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  X_HANDLE new_socket = SocketAccept(runtime::Current(), *socket, &native_addr, &native_len);
  if (new_socket != X_INVALID_HANDLE_VALUE) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
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
  uint32_t count;
  Pinned<SocketObject> sockets[64];

  void Load(const x_fd_set* guest_set) {
    REX_ASSERT(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == X_INVALID_SOCKET) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
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
    for (uint32_t i = 0; i < this->count; ++i) {
      guest_set->fd_array[guest_set->fd_count++] = this->sockets[i].handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle, native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
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
    i32 timeout_sec = static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]);
    i32 timeout_usec = static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1]);
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

  // TODO(gibbed): modify ret to be what's copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  return SocketRecv(*socket, buf_ptr, buf_len, flags);
}

u32 NetDll_recvfrom_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                          u32 buf_len, u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr,
                          mapped_u32 fromlen_ptr) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret = SocketRecvFrom(*socket, buf_ptr, buf_len, flags, &native_from,
                           fromlen_ptr ? &native_fromlen : 0);

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
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    ThreadRecord::SetLastError(error_code);
#else
    ThreadRecord::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                      u32 buf_len, u32 flags) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  return SocketSend(*socket, buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry([[maybe_unused]] u32 caller, u32 socket_handle, mapped_void buf_ptr,
                        u32 buf_len, u32 flags, ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = runtime::Current().object_table().Get<SocketObject>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    ThreadRecord::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return SocketSendTo(*socket, buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count =
      std::min(static_cast<uint32_t>(fd_set->fd_count), static_cast<uint32_t>(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  ThreadRecord::SetLastError(error_code);
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
