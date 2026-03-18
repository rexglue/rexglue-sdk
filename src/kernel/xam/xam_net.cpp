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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

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

XNetStartupParams xnet_startup_params = {0};

ppc_u32_result_t NetDll_XNetStartup_entry(ppc_u32_t caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

ppc_u32_result_t NetDll_XNetCleanup_entry(ppc_u32_t caller, ppc_pvoid_t params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

ppc_u32_result_t NetDll_XNetGetOpt_entry(ppc_u32_t one, ppc_u32_t option_id, ppc_pvoid_t buffer_ptr,
                                         ppc_pu32_t buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

ppc_u32_result_t NetDll_XNetRandom_entry(ppc_u32_t caller, ppc_pvoid_t buffer_ptr,
                                         ppc_u32_t length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

ppc_u32_result_t NetDll_WSAStartup_entry(ppc_u32_t caller, ppc_u16_t version,
                                         ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version.value();
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

ppc_u32_result_t NetDll_WSACleanup_entry(ppc_u32_t caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

ppc_u32_result_t NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

ppc_u32_result_t NetDll_WSARecvFrom_entry(ppc_u32_t caller, ppc_u32_t socket,
                                          ppc_ptr_t<XWSABUF> buffers_ptr, ppc_u32_t buffer_count,
                                          ppc_pu32_t num_bytes_recv, ppc_pu32_t flags_ptr,
                                          ppc_ptr_t<XSOCKADDR_IN> from_addr,
                                          ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                          ppc_pvoid_t completion_routine_ptr) {
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
ppc_u32_result_t NetDll_WSASendTo_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                        ppc_ptr_t<XWSABUF> buffers, ppc_u32_t num_buffers,
                                        ppc_pu32_t num_bytes_sent, ppc_u32_t flags,
                                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped,
                                        ppc_pvoid_t completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
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
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

ppc_u32_result_t NetDll_WSAWaitForMultipleEvents_entry(ppc_u32_t num_events, ppc_pu32_t events,
                                                       ppc_u32_t wait_all, ppc_u32_t timeout,
                                                       ppc_u32_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

ppc_u32_result_t NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

ppc_u32_result_t NetDll_WSACloseEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSAResetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSASetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
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
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

ppc_u32_result_t NetDll_XNetGetTitleXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
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

ppc_u32_result_t NetDll_XNetGetDebugXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

ppc_u32_result_t NetDll_XNetXnAddrToMachineId_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr,
                                                    ppc_pu32_t id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(ppc_u32_t caller, ppc_u32_t in_addr, ppc_pchar_t string_out,
                                     ppc_u32_t string_size) {
  rex::string::rex_strcpy(string_out, string_size, "666.666.666.666");
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
ppc_u32_result_t NetDll_XNetXnAddrToInAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> xn_addr,
                                                 ppc_pvoid_t xid, ppc_pvoid_t in_addr) {
  return 1;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
ppc_u32_result_t NetDll_XNetInAddrToXnAddr_entry(ppc_u32_t caller, ppc_pvoid_t in_addr,
                                                 ppc_ptr_t<XNADDR> xn_addr, ppc_pvoid_t xid) {
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
ppc_u32_result_t NetDll_XNetSetSystemLinkPort_entry(ppc_u32_t caller, ppc_u32_t port) {
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

ppc_u32_result_t NetDll_XNetGetEthernetLinkStatus_entry(ppc_u32_t caller) {
  return 0;
}

ppc_u32_result_t NetDll_XNetDnsLookup_entry(ppc_u32_t caller, ppc_pchar_t host,
                                            ppc_u32_t event_handle, ppc_pu32_t pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetDnsRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

ppc_u32_result_t NetDll_XNetQosServiceLookup_entry(ppc_u32_t caller, ppc_u32_t flags,
                                                   ppc_u32_t event_handle, ppc_pu32_t pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetQosRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

ppc_u32_result_t NetDll_XNetQosListen_entry(ppc_u32_t caller, ppc_pvoid_t id, ppc_pvoid_t data,
                                            ppc_u32_t data_size, ppc_u32_t r7, ppc_u32_t flags) {
  return X_ERROR_FUNCTION_FAILED;
}

ppc_u32_result_t NetDll_inet_addr_entry(ppc_pchar_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

ppc_u32_result_t NetDll_socket_entry(ppc_u32_t caller, ppc_u32_t af, ppc_u32_t type,
                                     ppc_u32_t protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

ppc_u32_result_t NetDll_closesocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

ppc_i32_result_t NetDll_shutdown_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

ppc_u32_result_t NetDll_setsockopt_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t level,
                                         ppc_u32_t optname, ppc_pvoid_t optval_ptr,
                                         ppc_u32_t optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

ppc_u32_result_t NetDll_ioctlsocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t cmd,
                                          ppc_pvoid_t arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

ppc_u32_result_t NetDll_bind_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                   ppc_ptr_t<XSOCKADDR_IN> name, ppc_u32_t namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_connect_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                      ppc_ptr_t<XSOCKADDR> name, ppc_u32_t namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_listen_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_accept_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                     ppc_ptr_t<XSOCKADDR> addr_ptr, ppc_pu32_t addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

ppc_i32_result_t NetDll_select_entry(ppc_i32_t caller, ppc_i32_t nfds, ppc_ptr_t<x_fd_set> readfds,
                                     ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                                     ppc_pvoid_t timeout_ptr) {
  host_set host_readfds = {0};
  fd_set native_readfds = {0};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {0};
  fd_set native_writefds = {0};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {0};
  fd_set native_exceptfds = {0};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
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

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

ppc_u32_result_t NetDll_recv_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_recvfrom_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                       ppc_pvoid_t buf_ptr, ppc_u32_t buf_len, ppc_u32_t flags,
                                       ppc_ptr_t<XSOCKADDR_IN> from_ptr, ppc_pu32_t fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
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
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

ppc_u32_result_t NetDll_send_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_sendto_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                     ppc_u32_t buf_len, ppc_u32_t flags,
                                     ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

ppc_u32_result_t NetDll___WSAFDIsSet_entry(ppc_u32_t socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(ppc_u32_t error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
XAM_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
XAM_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
XAM_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
XAM_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
XAM_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
XAM_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
XAM_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
XAM_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
XAM_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
XAM_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
XAM_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
XAM_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
XAM_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
XAM_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
XAM_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
XAM_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
XAM_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
XAM_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
XAM_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
XAM_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
XAM_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
XAM_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
XAM_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
XAM_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
XAM_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
XAM_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
XAM_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
XAM_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
XAM_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
XAM_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
XAM_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
XAM_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
XAM_EXPORT_STUB(__imp__NetDll_UpnpStartup);
XAM_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
XAM_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
XAM_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
XAM_EXPORT_STUB(__imp__NetDll_WSARecv);
XAM_EXPORT_STUB(__imp__NetDll_WSASend);
XAM_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_XHttpConnect);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpen);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReadData);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
XAM_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
XAM_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
XAM_EXPORT_STUB(__imp__NetDll_XHttpStartup);
XAM_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
XAM_EXPORT_STUB(__imp__NetDll_XNetConnect);
XAM_EXPORT_STUB(__imp__NetDll_XNetCreateKey);
XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetConnectStatus);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
XAM_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
XAM_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
XAM_EXPORT_STUB(__imp__NetDll_XNetQosLookup);
XAM_EXPORT_STUB(__imp__NetDll_XNetRegisterKey);
XAM_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
XAM_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
XAM_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetUnregisterKey);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
XAM_EXPORT_STUB(__imp__NetDll_XnpCapture);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfig);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
XAM_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
XAM_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_getpeername);
XAM_EXPORT_STUB(__imp__NetDll_getsockname);
XAM_EXPORT_STUB(__imp__NetDll_getsockopt);
