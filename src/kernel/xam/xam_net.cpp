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

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <span>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include "host_session_data.h"
#include "lan_system_link.h"
#include "live_sessions.h"
#include "relay_system_link.h"
#include "system_link_config.h"

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_STRING(system_link_local_address, "", "Online",
                      "Selected local IPv4 address for direct LAN System Link")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT64(system_link_xuid, 0, "Online",
                      "Local profile pseudonym used by direct LAN System Link")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(system_link_relay_address, "", "Online", "System Link relay IPv4 host:port")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(system_link_relay_room, "", "Online", "Private System Link relay room")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(system_link_relay_secret_file, "", "Online",
                      "Path to the System Link relay shared-secret file")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(system_link_advertised_address, "", "Online",
                      "Relay host:port to put in host invites instead of the connect address")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

bool RelayRequested() {
  return system_link::CurrentMode() == system_link::Mode::kRelay;
}

namespace {

lan::LanSystemLinkState& LanState() {
  static lan::LanSystemLinkState state;
  return state;
}

bool RelaySocket(uint32_t handle) {
  return RelayRequested() && relay_link::SystemLinkClient().IsDatagramSocket(handle);
}

std::mutex xnet_lifecycle;
// Titles keep sending on open sockets after an XNetCleanup and may call
// XNetStartup (including the Ex forms) more often than cleanup, so the relay
// association ends only at the last cleanup. Guarded by xnet_lifecycle.
system_link::RelayStartup relay_startup;

const char* SystemLinkMode() {
  return RelayRequested() ? "relay" : "direct LAN";
}

void RecordSystemLinkSend(int count) {
  if (count < 0 || !LanState().enabled())
    return;
  LanState().RecordSend(static_cast<size_t>(count));
  auto stats = LanState().stats_snapshot();
  if (stats.sent_packets == 1 || stats.sent_packets % 256 == 0)
    REXKRNL_DEBUG("sendto: {} packets={} bytes={}", SystemLinkMode(), stats.sent_packets,
                  stats.sent_bytes);
}

std::string Ipv4String(uint32_t raw_address) {
  in_addr address{};
  address.s_addr = raw_address;
  char text[16]{};
  return inet_ntop(AF_INET, &address, text, sizeof(text)) ? text : "<invalid>";
}

}  // namespace

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

namespace {

lan::XnAddrSnapshot LoadXnAddr(const XNADDR& address) {
  lan::XnAddrSnapshot snapshot{};
  snapshot.ipv4 = address.ina.s_addr;
  snapshot.online_ipv4 = address.inaOnline.s_addr;
  snapshot.online_port = address.wPortOnline;
  std::memcpy(snapshot.ethernet.data(), address.abEnet, snapshot.ethernet.size());
  std::memcpy(snapshot.online.data(), address.abOnline, snapshot.online.size());
  return snapshot;
}

void StoreXnAddr(const lan::XnAddrSnapshot& snapshot, XNADDR* address) {
  address->ina.s_addr = snapshot.ipv4;
  address->inaOnline.s_addr = snapshot.online_ipv4;
  address->wPortOnline = snapshot.online_port;
  std::memcpy(address->abEnet, snapshot.ethernet.data(), snapshot.ethernet.size());
  std::memcpy(address->abOnline, snapshot.online.data(), snapshot.online.size());
}

void FillRandomBytes(uint8_t* data, size_t size) {
  static std::mutex mutex;
  static std::mt19937_64 engine = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    return std::mt19937_64(seed);
  }();
  std::lock_guard lock(mutex);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(engine());
  }
}

}  // namespace

namespace lan {

LanSystemLinkState& SystemLinkState() {
  return LanState();
}

bool FillHostSessionInfo(const LanSystemLinkState& state, rex::memory::Memory& memory,
                         uint32_t flags, uint32_t session_info_ptr, uint32_t nonce_ptr) {
  if (!state.enabled() || !(flags & kSessionCreateHost) || !session_info_ptr) {
    return false;
  }
  // Snapshot once; a concurrent cleanup leaves a zero address that no client
  // could join.
  const XnAddrSnapshot local = state.local_xnaddr();
  if (!local.ipv4) {
    return false;
  }
  if (!memory.LookupHeap(session_info_ptr) ||
      !memory.LookupHeap(session_info_ptr + static_cast<uint32_t>(kSessionInfoSize) - 1) ||
      (nonce_ptr &&
       (!memory.LookupHeap(nonce_ptr) ||
        !memory.LookupHeap(nonce_ptr + static_cast<uint32_t>(kSessionNonceSize) - 1)))) {
    REXKRNL_WARN("XGISessionCreateImpl: LAN host fill skipped, bad guest pointer");
    return false;
  }

  // XSESSION_INFO: XNKID at +0, XNADDR at +8, XNKEY at +44.
  uint8_t info[kSessionInfoSize]{};
  FillRandomBytes(info, 8);
  // System-link XNKIDs have the top three bits of the first byte clear.
  info[0] &= 0x1F;
  XNADDR host_address{};
  StoreXnAddr(local, &host_address);
  static_assert(sizeof(XNADDR) == 36);
  std::memcpy(info + 8, &host_address, sizeof(host_address));
  FillRandomBytes(info + 44, 16);
  std::memcpy(memory.TranslateVirtual(session_info_ptr), info, sizeof(info));

  if (nonce_ptr) {
    uint8_t nonce[kSessionNonceSize]{};
    do {
      FillRandomBytes(nonce, sizeof(nonce));
    } while (std::all_of(nonce, nonce + sizeof(nonce), [](uint8_t b) { return b == 0; }));
    std::memcpy(memory.TranslateVirtual(nonce_ptr), nonce, sizeof(nonce));
  }

  uint64_t session_id = 0;
  for (int i = 0; i < 8; ++i) {
    session_id = (session_id << 8) | info[i];
  }
  REXKRNL_DEBUG("XGISessionCreateImpl: {} host session id={:016X} host={}", SystemLinkMode(),
                session_id, Ipv4String(local.ipv4));
  return true;
}

namespace {

bool IsSessionGuestRange(rex::memory::Memory& memory, uint32_t address, uint32_t size) {
  return address && size && address <= std::numeric_limits<uint32_t>::max() - (size - 1) &&
         memory.LookupHeap(address) && memory.LookupHeap(address + size - 1);
}

}  // namespace

void CreateSession(LanSystemLinkState& state, rex::memory::Memory& memory, uint32_t flags,
                   uint32_t public_slots, uint32_t private_slots, uint32_t user_index,
                   uint32_t session_info_ptr, uint32_t nonce_ptr) {
  if (!state.enabled()) {
    return;
  }
  if (!IsSessionGuestRange(memory, session_info_ptr, kSessionInfoSize) ||
      (nonce_ptr && !IsSessionGuestRange(memory, nonce_ptr, kSessionNonceSize))) {
    state.DeleteSession();
    return;
  }
  FillHostSessionInfo(state, memory, flags, session_info_ptr, nonce_ptr);
  SessionSnapshot session{};
  session.flags = flags;
  session.max_public_slots = public_slots;
  session.max_private_slots = private_slots;
  session.user_index = user_index;
  std::memcpy(session.info.data(), memory.TranslateVirtual(session_info_ptr), session.info.size());
  if (nonce_ptr) {
    std::memcpy(session.nonce.data(), memory.TranslateVirtual(nonce_ptr), session.nonce.size());
  }
  state.CreateSession(session);
}

void JoinSession(LanSystemLinkState& state, rex::memory::Memory& memory, uint32_t user_count,
                 uint32_t private_slots_ptr) {
  if (!state.enabled() || !state.session_snapshot().created || !user_count) {
    return;
  }
  uint32_t private_members = 0;
  if (private_slots_ptr) {
    if (user_count > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t) ||
        !IsSessionGuestRange(memory, private_slots_ptr, user_count * sizeof(uint32_t))) {
      return;
    }
    const auto* slots = memory.TranslateVirtual(private_slots_ptr);
    for (uint32_t i = 0; i < user_count; ++i) {
      if (memory::load_and_swap<uint32_t>(slots + i * sizeof(uint32_t))) {
        ++private_members;
      }
    }
  }
  state.JoinSession(user_count - private_members, private_members);
}

uint32_t FillSessionDetails(const LanSystemLinkState& state, rex::memory::Memory& memory,
                            uint32_t buffer_size_ptr, uint32_t details_ptr) {
  if (!state.enabled()) {
    return 0;
  }
  const auto session = state.session_snapshot();
  const uint32_t occupied_public = session.joined_public + session.filled_public;
  const uint32_t occupied_private = session.joined_private + session.filled_private;
  const uint32_t available_public =
      session.max_public_slots - std::min(session.max_public_slots, occupied_public);
  const uint32_t available_private =
      session.max_private_slots - std::min(session.max_private_slots, occupied_private);
  REXKRNL_DEBUG(
      "XSessionGetDetails: LAN created={} flags={:08X} public={}/{} private={}/{} joined={}/{}",
      session.created, session.flags, available_public, session.max_public_slots, available_private,
      session.max_private_slots, session.joined_public, session.joined_private);
  if (!session.created) {
    return 0;
  }
  if (!IsSessionGuestRange(memory, buffer_size_ptr, sizeof(uint32_t))) {
    return 87;  // ERROR_INVALID_PARAMETER
  }
  auto* buffer_size = memory.TranslateVirtual(buffer_size_ptr);
  if (memory::load_and_swap<uint32_t>(buffer_size) < kSessionDetailsSize) {
    memory::store_and_swap<uint32_t>(buffer_size, kSessionDetailsSize);
    return 122;  // ERROR_INSUFFICIENT_BUFFER
  }
  if (!IsSessionGuestRange(memory, details_ptr, kSessionDetailsSize)) {
    return 87;  // ERROR_INVALID_PARAMETER
  }
  auto* details = memory.TranslateVirtual(details_ptr);
  std::memset(details, 0, kSessionDetailsSize);
  memory::store_and_swap<uint32_t>(details + 0, session.user_index);
  memory::store_and_swap<uint32_t>(details + 12, session.flags);
  memory::store_and_swap<uint32_t>(details + 16, session.max_public_slots);
  memory::store_and_swap<uint32_t>(details + 20, session.max_private_slots);
  memory::store_and_swap<uint32_t>(details + 24, available_public);
  memory::store_and_swap<uint32_t>(details + 28, available_private);
  memory::store_and_swap<uint32_t>(details + 32, occupied_public + occupied_private);
  memory::store_and_swap<uint32_t>(details + 40, session.state);
  std::memcpy(details + 44, session.nonce.data(), session.nonce.size());
  std::memcpy(details + 52, session.info.data(), session.info.size());
  return 0;
}

}  // namespace lan

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

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  std::lock_guard lifecycle(xnet_lifecycle);
  const auto config = system_link::CurrentConfig();
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  if (RelayRequested()) {
    auto& client = relay_link::SystemLinkClient();
    client.SetProbeReplies(system_link::LiveSessionsActive());
    const auto result = relay_startup.Start(config, client, LanState());
    if (result)
      return result;
  } else if (!config.error.empty()) {
    // Like a failed relay: the title runs without a network and status gives the reason.
    REXKRNL_ERROR("XNetStartup: {}; continuing without a network", config.error);
    LanState().Reset();
  } else if (config.mode == system_link::Mode::kLan) {
    if (!LanState().Configure(config.local_address, config.xuid)) {
      REXKRNL_ERROR(
          "XNetStartup: direct LAN requires a selected IPv4 address and nonzero local XUID");
      return X_ERROR_INVALID_PARAMETER;
    }
    REXKRNL_DEBUG("XNetStartup: direct LAN address={} xuid={:016X}", config.local_address,
                  config.xuid);
  } else {
    LanState().Reset();
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

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  std::lock_guard lifecycle(xnet_lifecycle);
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  std::optional<relay_link::Stats> relay_stats;
  if (RelayRequested()) {
    relay_stats = relay_startup.Cleanup(relay_link::SystemLinkClient());
    if (!relay_stats) {
      REXKRNL_DEBUG("XNetCleanup: relay references={}", relay_startup.references());
      return 0;
    }
  }
  if (auto sessions = live::ActiveService())
    sessions->Cleanup();
  if (LanState().enabled()) {
    auto stats = LanState().stats_snapshot();
    auto qos = LanState().qos_snapshot();
    REXKRNL_DEBUG(
        "XNetCleanup: {} sent={} packets/{} bytes received={} packets/{} bytes "
        "qos_enabled={} qos_bytes={}",
        SystemLinkMode(), stats.sent_packets, stats.sent_bytes, stats.received_packets,
        stats.received_bytes, qos.enabled, qos.payload.size());
  }
  if (relay_stats) {
    const auto& stats = *relay_stats;
    REXKRNL_DEBUG(
        "XNetCleanup: relay state={} delivered={} unknown_port={} invalid_wire={} "
        "invalid_envelope={} rejected_destination={} queue_full={} oversize={} "
        "socket_errors={} connection_resets={} heartbeats={} discarded_pending={} "
        "dropped_after_cleanup={} direct_attempts={} direct_established={} "
        "direct_fallbacks={} direct_in={} direct_out={} direct_connect_timeouts={} "
        "direct_probe_failures={} direct_bad_tag={} direct_malformed={} direct_unknown_peer={} "
        "direct_stale_attempt={} direct_bad_mac={} direct_replay={} direct_fault_drops={} "
        "direct_evicted={}",
        static_cast<int>(stats.state), stats.delivered, stats.unknown_port, stats.invalid_wire,
        stats.invalid_envelope, stats.rejected_destination, stats.queue_full, stats.oversize,
        stats.socket_errors, stats.connection_resets, stats.heartbeats, stats.discarded_pending,
        stats.dropped_after_cleanup, stats.direct_attempts, stats.direct_established,
        stats.direct_fallbacks, stats.direct_in, stats.direct_out, stats.direct_connect_timeouts,
        stats.direct_probe_failures, stats.direct_bad_tag, stats.direct_malformed,
        stats.direct_unknown_peer, stats.direct_stale_attempt, stats.direct_bad_mac,
        stats.direct_replay, stats.direct_fault_drops, stats.direct_evicted);
  }
  host_data::OnNetworkCleanup();
  LanState().Reset();
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
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

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
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
    data_ptr->version = version;
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

// The Ex forms ignore the caller's XNet library version. Titles reach them
// through the System Link system version; with System Link off, they return
// the caller value in r3 without running startup.
u32 NetDll_XNetStartupEx_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params, u32 version) {
  if (system_link::CurrentConfig().mode == system_link::Mode::kOff) {
    REXKRNL_WARN("__imp__NetDll_XNetStartupEx STUB");
    return caller;
  }
  return NetDll_XNetStartup_entry(caller, params);
}

u32 NetDll_WSAStartupEx_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr,
                              u32 library_version) {
  if (system_link::CurrentConfig().mode == system_link::Mode::kOff) {
    REXKRNL_WARN("__imp__NetDll_WSAStartupEx STUB");
    return caller;
  }
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
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
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
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
    if (RelaySocket(socket_handle) && buffers[i].len > relay::kMaxPayload - combined_buffer_size) {
      XThread::SetLastError(relay_link::kMessageSize);
      return -1;
    }
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  if (RelaySocket(socket_handle)) {
    if (!to_ptr || to_len < sizeof(XSOCKADDR_IN) || to_ptr->sin_family != XSocket::X_AF_INET) {
      XThread::SetLastError(10022);
      return -1;
    }
    auto result = relay_link::SystemLinkClient().Send(
        socket->native_handle(), {htonl(static_cast<uint32_t>(to_ptr->sin_addr)), to_ptr->sin_port},
        combined_buffer_mem);
    if (!result) {
      XThread::SetLastError(result.error());
      return -1;
    }
    if (num_bytes_sent)
      *num_bytes_sent = *result;
    RecordSystemLinkSend(*result);
    return 0;
  }
  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
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

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
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

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (RelayRequested()) {
    lan::XnAddrSnapshot address{};
    const auto status =
        system_link::RelayTitleXnAddr(relay_link::SystemLinkClient(), LanState(), &address);
    StoreXnAddr(address, addr_ptr);
    REXKRNL_DEBUG("XNetGetTitleXnAddr: relay address={} status={}", Ipv4String(address.ipv4),
                  status);
    return status;
  }
  if (LanState().enabled()) {
    StoreXnAddr(LanState().local_xnaddr(), addr_ptr);
    REXKRNL_DEBUG("XNetGetTitleXnAddr: {} address={}", SystemLinkMode(),
                  Ipv4String(LanState().local_ipv4()));
    return XnAddrStatus::XNET_GET_XNADDR_STATIC;
  }

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

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (LanState().enabled()) {
    if (!xn_addr || !in_addr) {
      REXKRNL_DEBUG("XNetXnAddrToInAddr: {} invalid parameter (xnaddr={} inaddr={})",
                    SystemLinkMode(), xn_addr ? "set" : "null", in_addr ? "set" : "null");
      return X_ERROR_INVALID_PARAMETER;
    }
    uint32_t converted = 0;
    const lan::XnAddrSnapshot peer = LoadXnAddr(*xn_addr);
    if (!LanState().XnAddrToInAddr(peer, &converted)) {
      REXKRNL_DEBUG("XNetXnAddrToInAddr: {} conversion failed, ina={}", SystemLinkMode(),
                    Ipv4String(peer.ipv4));
      return X_ERROR_FUNCTION_FAILED;
    }
    std::memcpy(in_addr.host_address(), &converted, sizeof(converted));
    REXKRNL_DEBUG("XNetXnAddrToInAddr: {} address={}", SystemLinkMode(), Ipv4String(converted));
    return 0;
  }
  return 1;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, mapped_void in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (LanState().enabled()) {
    if (!in_addr || !xn_addr) {
      return X_ERROR_INVALID_PARAMETER;
    }
    uint32_t converted = 0;
    std::memcpy(&converted, in_addr.host_address(), sizeof(converted));
    lan::XnAddrSnapshot snapshot{};
    if (!LanState().InAddrToXnAddr(converted, &snapshot)) {
      return X_ERROR_FUNCTION_FAILED;
    }
    StoreXnAddr(snapshot, xn_addr);
    REXKRNL_DEBUG("XNetInAddrToXnAddr: {} address={}", SystemLinkMode(), Ipv4String(converted));
    return 0;
  }
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
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

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
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

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
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

// XNetQosLookup(cxna, apxna, apxnkid, apxnkey, cina, aina, adwServiceId, cProbes,
// dwBitsPerSec, dwFlags, hEvent, ppxnqos) with the NetDll caller first. The
// export runs this only while Live sessions are active.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 count, u32 addresses_ptr, u32 ids_ptr, u32 keys_ptr,
                               u32 server_count, u32 servers_ptr, u32 service_ids_ptr, u32 probes,
                               u32 bits_per_second, u32 flags, u32 event_handle, u32 result_ptr) {
  auto sessions = live::ActiveService();
  if (!sessions)
    return X_ERROR_FUNCTION_FAILED;
  return live::HandleQosLookup(*sessions, *REX_KERNEL_MEMORY(), count, addresses_ptr, ids_ptr,
                               server_count, probes, event_handle, result_ptr);
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Stop writing a lookup's results before its block is freed.
  if (auto sessions = live::ActiveService())
    sessions->ReleaseQos(qos.guest_address());
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  if (!LanState().enabled() && !RelayRequested()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (data_size && !data) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto payload = data_size ? std::span<const uint8_t>(data.as<const uint8_t*>(), data_size)
                           : std::span<const uint8_t>();
  const auto result =
      system_link::QosListen(LanState(), system_link::CurrentMode(), payload, r7, flags);
  if (result == 0) {
    if (auto sessions = live::ActiveService()) {
      const auto qos = LanState().qos_snapshot();
      sessions->SetQosListen(
          qos.enabled ? std::span<const uint8_t>(qos.payload) : std::span<const uint8_t>(),
          live::Clock::now());
    }
  }
  return result;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
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

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
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

  auto kind = lan::SocketKind::kStream;
  if (static_cast<uint32_t>(type) == XSocket::X_SOCK_DGRAM) {
    kind = static_cast<uint32_t>(protocol) == XSocket::X_IPPROTO_VDP ? lan::SocketKind::kVdp
                                                                     : lan::SocketKind::kDatagram;
  }
  LanState().RegisterSocket(socket->handle(), kind);
  if (RelayRequested())
    relay_link::SystemLinkClient().RegisterSocket(socket->handle(),
                                                  kind != lan::SocketKind::kStream);
  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  relay_link::SystemLinkClient().Release(socket->native_handle());
  relay_link::SystemLinkClient().ForgetSocket(socket_handle);
  socket->Close();
  LanState().ForgetSocket(socket_handle);
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
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

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
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

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    if (!name || namelen < sizeof(XSOCKADDR_IN) || name->sin_family != XSocket::X_AF_INET) {
      XThread::SetLastError(10022);
      return -1;
    }
    auto result = relay_link::SystemLinkClient().Bind(socket->native_handle(), name->sin_port);
    if (!result) {
      XThread::SetLastError(result.error());
      return -1;
    }
    LanState().MarkBound(socket_handle);
    REXKRNL_DEBUG("bind: relay socket={} address={} port={}", socket_handle,
                  Ipv4String(relay_link::SystemLinkClient().ipv4()),
                  relay_link::SystemLinkClient().GuestPort(socket->native_handle()));
    return 0;
  }
  N_XSOCKADDR_IN native_name(name);
  const bool lan_enabled = LanState().enabled() && !RelayRequested();
  bool took_port_claim = false;
  if (lan_enabled) {
    const uint32_t requested_address = htonl(static_cast<uint32_t>(name->sin_addr));
    native_name.sin_addr = ntohl(LanState().SelectBindIpv4(requested_address));
    const auto port = static_cast<uint16_t>(native_name.sin_port);
    // Plain datagram ports receive limited broadcasts from every local
    // instance, so one machine-wide owner keeps broadcast-addressed traffic
    // unambiguous. VDP ports stay per address.
    if (!requested_address && port &&
        LanState().socket_kind(socket_handle) == lan::SocketKind::kDatagram) {
      if (const int claim_error =
              LanState().ClaimDatagramPort(socket_handle, port, &took_port_claim)) {
        XThread::SetLastError(static_cast<uint32_t>(claim_error));
        REXKRNL_DEBUG("bind: direct LAN socket={} port={} claimed elsewhere error={}",
                      socket_handle, port, claim_error);
        return -1;
      }
    }
    REXKRNL_DEBUG("bind: direct LAN socket={} address={} port={}", socket_handle,
                  Ipv4String(htonl(static_cast<uint32_t>(native_name.sin_addr))), port);
  }
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
#if REX_PLATFORM_WIN32
    const auto native_error = static_cast<uint32_t>(WSAGetLastError());
#else
    const auto native_error = xboxkrnl::xeRtlNtStatusToDosError(status);
#endif
    if (took_port_claim) {
      LanState().ReleasePortClaim(socket_handle);
    }
    if (lan_enabled) {
      XThread::SetLastError(native_error);
      REXKRNL_ERROR("bind: direct LAN socket={} failed error={}", socket_handle, native_error);
    } else {
      XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    }
    return -1;
  }

  LanState().MarkBound(socket_handle);
  return 0;
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 name_len) {
  if (!name || !name_len || *name_len < sizeof(XSOCKADDR_IN)) {
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

  N_XSOCKADDR_IN native_name{};
  uint32_t native_name_len = *name_len;
  X_STATUS status = socket->GetSockName(&native_name, &native_name_len);
  if (XFAILED(status)) {
#if REX_PLATFORM_WIN32
    XThread::SetLastError(static_cast<uint32_t>(WSAGetLastError()));
#else
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
#endif
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    native_name.sin_addr = ntohl(relay_link::SystemLinkClient().ipv4());
    native_name.sin_port = relay_link::SystemLinkClient().GuestPort(socket->native_handle());
  }
  name->sin_family = native_name.sin_family;
  name->sin_port = native_name.sin_port;
  name->sin_addr = native_name.sin_addr;
  std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  *name_len = native_name_len;

  if (LanState().enabled()) {
    REXKRNL_DEBUG("getsockname: {} socket={} address={} port={}", SystemLinkMode(), socket_handle,
                  Ipv4String(htonl(static_cast<uint32_t>(native_name.sin_addr))),
                  static_cast<uint16_t>(native_name.sin_port));
  }
  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    XThread::SetLastError(10045);  // WSAEOPNOTSUPP: connected relay UDP is unsupported.
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

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
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

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
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

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
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

// Report physical-view receive length as min(received, buffer size); if
// truncation fills the buffer before failure, report the full buffer rather
// than datagram length.
static void NotifyReceived(uint32_t physical, uint32_t buf_len, int ret, int error) {
  if (physical == UINT32_MAX) {
    return;
  }
  const uint32_t copied = ret > 0 ? std::min(uint32_t(ret), buf_len)
                          : ret < 0 && error == relay_link::kMessageSize ? buf_len
                                                                         : 0;
  if (copied) {
    REX_KERNEL_MEMORY()->NotifyPhysicalWrite(physical, copied);
  }
}

// The native socket error of the last failed call, where the platform has one.
static int LastReceiveError() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError();
#else
  return 0;
#endif
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    XThread::SetLastError(10045);  // Keep raw loopback envelopes inside recvfrom.
    return -1;
  }
  uint32_t physical = UINT32_MAX;
  auto* buffer =
      REX_KERNEL_MEMORY()->TranslateForHostWrite(buf_ptr.guest_address(), buf_len, &physical);
  const int ret = socket->Recv(buffer, buf_len, flags);
  if (ret < 0) {
    const int error = LastReceiveError();
    NotifyReceived(physical, buf_len, ret, error);
#if REX_PLATFORM_WIN32
    WSASetLastError(error);
#endif
  } else {
    NotifyReceived(physical, buf_len, ret, 0);
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
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
  uint32_t physical = UINT32_MAX;
  auto* buffer =
      REX_KERNEL_MEMORY()->TranslateForHostWrite(buf_ptr.guest_address(), buf_len, &physical);
  int ret;
  if (RelaySocket(socket_handle)) {
    auto result = relay_link::SystemLinkClient().ReceiveFrom(
        socket->native_handle(), {buffer, static_cast<uint32_t>(buf_len)}, flags, native_from,
        native_fromlen);
    if (!result) {
      NotifyReceived(physical, buf_len, -1, result.error());
      XThread::SetLastError(result.error());
      return -1;
    }
    ret = *result;
    NotifyReceived(physical, buf_len, ret, 0);
  } else {
    ret = socket->RecvFrom(buffer, buf_len, flags, &native_from,
                           fromlen_ptr ? &native_fromlen : nullptr);
    if (ret < 0) {
      const int error = LastReceiveError();
      NotifyReceived(physical, buf_len, ret, error);
#if REX_PLATFORM_WIN32
      WSASetLastError(error);
#endif
    } else {
      NotifyReceived(physical, buf_len, ret, 0);
    }
  }

  if (ret >= 0 && LanState().enabled()) {
    LanState().RecordReceive(static_cast<size_t>(ret));
    auto stats = LanState().stats_snapshot();
    if (stats.received_packets == 1 || (stats.received_packets % 256) == 0) {
      REXKRNL_DEBUG("recvfrom: {} packets={} bytes={}", SystemLinkMode(), stats.received_packets,
                    stats.received_bytes);
    }
  }

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

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    XThread::SetLastError(10045);  // A datagram must supply its virtual destination to sendto.
    return -1;
  }
  return socket->Send(buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (RelaySocket(socket_handle)) {
    if (!to_ptr || to_len < sizeof(XSOCKADDR_IN) || to_ptr->sin_family != XSocket::X_AF_INET) {
      XThread::SetLastError(10022);
      return -1;
    }
    auto result = relay_link::SystemLinkClient().Send(
        socket->native_handle(), {htonl(static_cast<uint32_t>(to_ptr->sin_addr)), to_ptr->sin_port},
        {static_cast<uint8_t*>(buf_ptr), static_cast<uint32_t>(buf_len)});
    if (!result) {
      XThread::SetLastError(result.error());
      return -1;
    }
    RecordSystemLinkSend(*result);
    return *result;
  }
  if (!RelayRequested() && LanState().NeedsBindBeforeSend(socket_handle)) {
    if (lan::BindUnboundToLocalAddress(*socket, LanState().local_ipv4())) {
      N_XSOCKADDR_IN bound{};
      uint32_t bound_size = sizeof(bound);
      socket->GetSockName(&bound, &bound_size);
      REXKRNL_DEBUG("sendto: direct LAN socket={} bound address={} port={}", socket_handle,
                    Ipv4String(htonl(static_cast<uint32_t>(bound.sin_addr))),
                    static_cast<uint16_t>(bound.sin_port));
    }
    LanState().MarkBound(socket_handle);
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  RecordSystemLinkSend(ret);
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetStartupEx, rex::kernel::xam::NetDll_XNetStartupEx_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSAStartupEx, rex::kernel::xam::NetDll_WSAStartupEx_entry)
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
// Without Live sessions, return before argument translation to preserve every
// guest register, including r3's upper word.
extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup) {
  if (!rex::kernel::xam::live::ActiveService()) {
    (void)base;
    REXKRNL_WARN("{} STUB", "__imp__NetDll_XNetQosLookup");
    return;
  }
  rex::ppc::HostToGuestFunction<rex::kernel::xam::NetDll_XNetQosLookup_entry>(ctx, base);
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__NetDll_XNetQosLookup(
    "__imp__NetDll_XNetQosLookup", &__imp__NetDll_XNetQosLookup);
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
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
REX_EXPORT_STUB(__imp__NetDll_XNetRegisterKey);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
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
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
