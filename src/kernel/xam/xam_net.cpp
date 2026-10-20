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
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/xnet.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/net/socket.h>
#include <rex/types.h>
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
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_STRING(net_interface, "", "Net",
                      "System link: the network to play on. Empty picks the interface of the "
                      "default route; otherwise an IPv4 address of this PC or an interface name "
                      "(names on Linux only)");
REXCVAR_DEFINE_BOOL(net_lan_only, true, "Net",
                    "System link: exchange packets only with local network addresses (private, "
                    "link-local and the chosen interface's subnet). Turn off to play over the "
                    "internet, for example with a forwarded port");

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
// IN_ADDR values are big-endian in guest memory; here they are handled in
// host byte order (192.168.1.2 is 0xC0A80102), as the guest's registers
// hold them.
using XNADDR = X_XNADDR;

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

//=============================================================================
// System link (LAN) state and rules
//=============================================================================
// Security rules for everything below:
// - Every pointer and length from the guest is checked against mapped guest
//   memory (GuestRangeValid) before the host reads or writes through it.
// - Nothing from the network is trusted: a datagram's length is the host's,
//   never a field inside it; VDP headers are checked before the guest sees
//   them; a datagram too big for the guest's buffer fails with WSAEMSGSIZE.
// - Sockets bind only where the guest asks. With net_lan_only (the default)
//   packets are exchanged only with local network addresses.
// - Nothing here reads files or runs commands.

using rex::net::SockAddrV4;
namespace wsa = rex::net::wsa;

namespace {

// XNetGetConnectStatus results.
constexpr uint32_t kConnectStatusConnected = 2;
constexpr uint32_t kConnectStatusLost = 3;

// XNQOSINFO flags.
constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;

// QoS has no probe between PCs: a reachable target is reported at this
// round trip time and bandwidth.
constexpr uint16_t kQosRttMs = 10;
constexpr uint32_t kQosBitsPerSec = 10000000;

// Table limits; each table drops its oldest entry when full.
constexpr size_t kMaxKeys = 64;
constexpr size_t kMaxPeers = 256;
constexpr size_t kMaxQosBlocks = 256;
constexpr size_t kMaxWarnedAddresses = 64;

// Largest UDP payload over IPv4.
constexpr uint32_t kMaxDatagram = 65507;

struct TitleAddress {
  uint32_t addr = rex::net::kIpv4Loopback;
  uint32_t netmask = 0xFF000000;
  bool broadcast = false;
  bool lan = false;  // false when only the loopback address was found
};

struct KeyEntry {
  X_XNKID kid;
  X_XNKEY key;
};

struct PeerEntry {
  uint32_t ina = 0;  // host byte order
  X_XNADDR xnaddr;
  X_XNKID kid;
  uint64_t last_used = 0;
};

struct XNetState {
  std::mutex mutex;
  TitleAddress title;
  std::string title_choice;  // net_interface when `title` was picked
  std::chrono::steady_clock::time_point title_time;
  bool title_valid = false;
  std::vector<KeyEntry> keys;  // oldest first
  X_XNKID created_kid = {};    // the newest XNetCreateKey result
  bool has_created_kid = false;
  std::vector<PeerEntry> peers;
  uint64_t clock = 0;
  std::deque<uint32_t> qos_blocks;  // XNQOS results, oldest first
  std::set<uint32_t> warned;        // addresses already logged as blocked
};

XNetState& State() {
  static XNetState state;
  return state;
}

}  // namespace

namespace xnet {

bool GuestRangeValid(uint32_t address, uint32_t length, bool write) {
  if (!address) {
    return false;
  }
  if (!length) {
    return true;
  }
  uint64_t last = uint64_t(address) + length - 1;
  if (last > 0xFFFFFFFFull) {
    return false;
  }
  auto heap = REX_KERNEL_MEMORY()->LookupHeap(address);
  if (!heap) {
    return false;
  }
  auto access = heap->QueryRangeAccess(address, static_cast<uint32_t>(last));
  if (write) {
    return access == rex::memory::PageAccess::kReadWrite;
  }
  return access != rex::memory::PageAccess::kNoAccess;
}

}  // namespace xnet

using xnet::GuestRangeValid;

namespace {

uint8_t* GuestBytes(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
}

// Picks the interface system link uses. `note` says how, for the log.
TitleAddress PickTitleAddress(const std::string& choice, std::string* note) {
  auto interfaces = rex::net::list_ipv4_interfaces();
  std::vector<rex::net::Ipv4Interface> usable;
  for (auto& entry : interfaces) {
    if (entry.up && !entry.loopback && entry.address &&
        !rex::net::ipv4_is_loopback(entry.address)) {
      usable.push_back(entry);
    }
  }
  auto make = [](const rex::net::Ipv4Interface& entry) {
    TitleAddress title;
    title.addr = entry.address;
    title.netmask = entry.netmask;
    title.broadcast = entry.broadcast;
    title.lan = true;
    return title;
  };
  if (!choice.empty()) {
    uint32_t wanted = 0;
    bool by_address = rex::net::parse_ipv4(choice, &wanted);
    for (auto& entry : usable) {
      if (by_address ? entry.address == wanted : entry.name == choice) {
        *note = "net_interface";
        return make(entry);
      }
    }
    *note = "net_interface '" + choice + "' is not an active interface of this PC; ";
  }
  // The interface of the default route, then a private one, then any.
  uint32_t route = rex::net::default_route_ipv4();
  const rex::net::Ipv4Interface* best = nullptr;
  for (auto& entry : usable) {
    if (entry.address == route) {
      best = &entry;
      break;
    }
  }
  for (auto& entry : usable) {
    if (!best && rex::net::ipv4_is_private(entry.address) && entry.broadcast) {
      best = &entry;
    }
  }
  if (!best && !usable.empty()) {
    best = &usable.front();
  }
  if (!best) {
    *note += "no network interface is up, so system link only reaches this PC";
    return TitleAddress();
  }
  *note += "picked automatically; set net_interface to choose another";
  return make(*best);
}

// The chosen address, looked up again every few seconds or when
// net_interface changes. Called with the state locked.
const TitleAddress& CurrentTitle(XNetState& state) {
  auto now = std::chrono::steady_clock::now();
  std::string choice = REXCVAR_GET(net_interface);
  bool choice_changed = !state.title_valid || choice != state.title_choice;
  if (choice_changed || now - state.title_time > std::chrono::seconds(5)) {
    std::string note;
    TitleAddress title = PickTitleAddress(choice, &note);
    if (choice_changed || title.addr != state.title.addr) {
      REXKRNL_INFO("XNet: system link address {} ({})", rex::net::format_ipv4(title.addr), note);
    }
    state.title = title;
    state.title_choice = choice;
    state.title_time = now;
    state.title_valid = true;
  }
  return state.title;
}

uint32_t DirectedBroadcast(const TitleAddress& title) {
  if (!title.lan || !title.broadcast) {
    return rex::net::kIpv4Broadcast;
  }
  return rex::net::ipv4_directed_broadcast(title.addr, title.netmask);
}

// Whether system link may exchange packets with `addr`. Called with the
// state locked.
bool PeerAllowedLocked(XNetState& state, uint32_t addr) {
  if (!REXCVAR_GET(net_lan_only)) {
    return addr && !rex::net::ipv4_is_multicast(addr);
  }
  const TitleAddress& title = CurrentTitle(state);
  return rex::net::ipv4_is_lan_peer(addr, title.lan ? title.addr : 0, title.netmask);
}

bool PeerAllowed(uint32_t addr, const char* what) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (PeerAllowedLocked(state, addr)) {
    return true;
  }
  if (state.warned.size() < kMaxWarnedAddresses && state.warned.insert(addr).second) {
    REXKRNL_WARN("XNet: {} {} blocked: not a local network address (net_lan_only)", what,
                 rex::net::format_ipv4(addr));
  }
  return false;
}

// The XNADDR of the PC at `ina`. The console's MAC address field is how the
// game tells players apart, so it is made from the address: every PC
// computes the same XNADDR for a peer from its address alone.
X_XNADDR MakeXnAddr(uint32_t ina) {
  X_XNADDR xnaddr;
  std::memset(&xnaddr, 0, sizeof(xnaddr));
  xnaddr.ina = ina;
  xnaddr.ina_online = 0;
  xnaddr.port_online = 0;
  // A locally administered, unicast MAC address.
  xnaddr.enet[0] = 0x02;
  xnaddr.enet[1] = 0x00;
  xnaddr.enet[2] = uint8_t(ina >> 24);
  xnaddr.enet[3] = uint8_t(ina >> 16);
  xnaddr.enet[4] = uint8_t(ina >> 8);
  xnaddr.enet[5] = uint8_t(ina);
  return xnaddr;
}

PeerEntry* FindPeer(XNetState& state, uint32_t ina) {
  for (auto& peer : state.peers) {
    if (peer.ina == ina) {
      return &peer;
    }
  }
  return nullptr;
}

PeerEntry& AddPeer(XNetState& state, uint32_t ina, const X_XNADDR& xnaddr, const X_XNKID& kid) {
  PeerEntry* peer = FindPeer(state, ina);
  if (!peer) {
    if (state.peers.size() >= kMaxPeers) {
      // Forget the peer used longest ago.
      auto oldest = std::min_element(
          state.peers.begin(), state.peers.end(),
          [](const PeerEntry& a, const PeerEntry& b) { return a.last_used < b.last_used; });
      state.peers.erase(oldest);
    }
    state.peers.emplace_back();
    peer = &state.peers.back();
    peer->ina = ina;
  }
  peer->xnaddr = xnaddr;
  peer->kid = kid;
  peer->last_used = ++state.clock;
  return *peer;
}

void SetError(uint32_t error) {
  XThread::SetLastError(error);
}

object_ref<XSocket> LookupSocket(uint32_t socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    SetError(wsa::kEnotsock);
  }
  return socket;
}

// Reads a guest sockaddr_in. Returns 0 or a Winsock error code.
uint32_t ReadGuestSockaddr(uint32_t address, uint32_t length, SockAddrV4* out) {
  if (length < sizeof(XSOCKADDR_IN) || !GuestRangeValid(address, sizeof(XSOCKADDR_IN), false)) {
    return wsa::kEfault;
  }
  const uint8_t* bytes = GuestBytes(address);
  if (memory::load_and_swap<uint16_t>(bytes + 0) != XSocket::X_AF_INET) {
    return wsa::kEafnosupport;
  }
  out->port = memory::load_and_swap<uint16_t>(bytes + 2);
  out->addr = memory::load_and_swap<uint32_t>(bytes + 4);
  return 0;
}

// Writes a guest sockaddr_in and its length (16). `length_address` holds
// the buffer's size on entry. Returns 0 or a Winsock error code.
uint32_t CheckGuestSockaddrOut(uint32_t address, uint32_t length_address) {
  if (!address) {
    return 0;
  }
  if (!GuestRangeValid(length_address, 4, true)) {
    return wsa::kEfault;
  }
  uint32_t length = memory::load_and_swap<uint32_t>(GuestBytes(length_address));
  if (length < sizeof(XSOCKADDR_IN) || !GuestRangeValid(address, sizeof(XSOCKADDR_IN), true)) {
    return wsa::kEfault;
  }
  return 0;
}

void WriteGuestSockaddr(uint32_t address, uint32_t length_address, const SockAddrV4& addr) {
  if (!address) {
    return;
  }
  uint8_t* bytes = GuestBytes(address);
  memory::store_and_swap<uint16_t>(bytes + 0, XSocket::X_AF_INET);
  memory::store_and_swap<uint16_t>(bytes + 2, addr.port);
  memory::store_and_swap<uint32_t>(bytes + 4, addr.addr);
  std::memset(bytes + 8, 0, 8);
  memory::store_and_swap<uint32_t>(GuestBytes(length_address), sizeof(XSOCKADDR_IN));
}

// A VDP datagram starts with the game data's length (2 bytes, big-endian),
// which can't be more than what follows it.
bool VdpHeaderValid(const uint8_t* data, uint32_t length) {
  if (length < 2) {
    return false;
  }
  uint32_t game_data_length = memory::load_and_swap<uint16_t>(data);
  return game_data_length <= length - 2;
}

// sendto for datagram sockets: checks the destination and the VDP header.
// Returns the bytes sent, or -1 with the guest's last error set.
int SendDatagram(XSocket* socket, const uint8_t* data, uint32_t length, uint32_t flags,
                 const SockAddrV4* to) {
  uint32_t error = 0;
  if (socket->type() != XSocket::X_SOCK_DGRAM) {
    int ret = socket->Send(data, length, flags, &error);
    if (ret < 0) {
      SetError(error);
    }
    return ret;
  }
  if (length > kMaxDatagram) {
    SetError(wsa::kEmsgsize);
    return -1;
  }
  if (socket->is_vdp() && !VdpHeaderValid(data, length)) {
    SetError(wsa::kEinval);
    return -1;
  }
  SockAddrV4 target;
  if (to) {
    target = *to;
    if (target.addr == rex::net::kIpv4Broadcast) {
      if (!socket->is_broadcast()) {
        SetError(wsa::kEacces);
        return -1;
      }
      // Send the broadcast on the chosen interface's network.
      auto& state = State();
      std::lock_guard<std::mutex> lock(state.mutex);
      target.addr = DirectedBroadcast(CurrentTitle(state));
    }
    if (!PeerAllowed(target.addr, "send to")) {
      SetError(wsa::kEhostunreach);
      return -1;
    }
  }
  int ret = socket->SendTo(data, length, flags, to ? &target : nullptr, &error);
  if (ret < 0) {
    SetError(error);
  }
  return ret;
}

}  // namespace

namespace xnet {

void GetTitleXnAddr(X_XNADDR* out) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  *out = MakeXnAddr(CurrentTitle(state).addr);
}

void CreateKey(X_XNKID* kid, X_XNKEY* key) {
  if (!rex::net::random_bytes(kid->ab, sizeof(kid->ab)) ||
      !rex::net::random_bytes(key->ab, sizeof(key->ab))) {
    REXKRNL_ERROR("XNet: the system's random source failed");
  }
  // The top bits of an XNKID give its kind: 0 is a system link session.
  kid->ab[0] &= 0x0F;
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.created_kid = *kid;
  state.has_created_kid = true;
}

uint32_t RegisterKey(const X_XNKID& kid, const X_XNKEY& key) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  // Registering again moves the key to the newest place.
  state.keys.erase(std::remove_if(state.keys.begin(), state.keys.end(),
                                  [&](const KeyEntry& entry) {
                                    return !std::memcmp(entry.kid.ab, kid.ab, sizeof(kid.ab));
                                  }),
                   state.keys.end());
  if (state.keys.size() >= kMaxKeys) {
    state.keys.erase(state.keys.begin());
  }
  state.keys.push_back({kid, key});
  return 0;
}

}  // namespace xnet

u32 NetDll_XNetStartup_entry(u32 caller, u32 params_ptr) {
  if (params_ptr && GuestRangeValid(params_ptr, sizeof(XNetStartupParams), false)) {
    auto params = GuestBytes(params_ptr);
    if (params[0] == sizeof(XNetStartupParams)) {
      std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
    }
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
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.keys.clear();
  state.peers.clear();
  state.has_created_kid = false;
  state.title_valid = false;
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, u32 buffer_ptr, u32 buffer_size_ptr) {
  if (!GuestRangeValid(buffer_size_ptr, 4, true)) {
    return wsa::kEfault;
  }
  auto buffer_size = GuestBytes(buffer_size_ptr);
  switch (option_id) {
    case 1:
      if (memory::load_and_swap<uint32_t>(buffer_size) < sizeof(XNetStartupParams)) {
        memory::store_and_swap<uint32_t>(buffer_size, sizeof(XNetStartupParams));
        return wsa::kEmsgsize;
      }
      if (!GuestRangeValid(buffer_ptr, sizeof(XNetStartupParams), true)) {
        return wsa::kEfault;
      }
      std::memcpy(GuestBytes(buffer_ptr), &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return wsa::kEinval;
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, u32 buffer_ptr, u32 length) {
  if (!length) {
    return 0;
  }
  if (!GuestRangeValid(buffer_ptr, length, true)) {
    return wsa::kEfault;
  }
  // The game makes session nonces and keys from these: they must differ
  // between players.
  if (!rex::net::random_bytes(GuestBytes(buffer_ptr), length)) {
    REXKRNL_ERROR("XNet: the system's random source failed");
    return wsa::kEinval;
  }
  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
  if (data_ptr && !GuestRangeValid(data_ptr.guest_address(), sizeof(X_WSADATA), true)) {
    return wsa::kEfault;
  }
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
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 num_buffers,
                           u32 num_bytes_sent_ptr, u32 flags, u32 to_ptr, u32 to_len,
                           u32 overlapped, u32 completion_routine) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!num_buffers || num_buffers > 64 ||
      !GuestRangeValid(buffers_ptr, num_buffers * sizeof(XWSABUF), false)) {
    SetError(wsa::kEfault);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  auto buffers = reinterpret_cast<const XWSABUF*>(GuestBytes(buffers_ptr));
  std::vector<uint8_t> combined;
  for (uint32_t i = 0; i < num_buffers; i++) {
    uint32_t len = buffers[i].len;
    uint32_t ptr = buffers[i].buf_ptr;
    if (len > kMaxDatagram - combined.size()) {
      SetError(wsa::kEmsgsize);
      return -1;
    }
    if (len && !GuestRangeValid(ptr, len, false)) {
      SetError(wsa::kEfault);
      return -1;
    }
    const uint8_t* data = len ? GuestBytes(ptr) : nullptr;
    combined.insert(combined.end(), data, data + len);
  }

  SockAddrV4 to;
  if (to_ptr) {
    uint32_t error = ReadGuestSockaddr(to_ptr, to_len, &to);
    if (error) {
      SetError(error);
      return -1;
    }
  }
  int ret = SendDatagram(socket.get(), combined.data(), uint32_t(combined.size()), flags,
                         to_ptr ? &to : nullptr);
  if (ret < 0) {
    return -1;
  }
  if (num_bytes_sent_ptr && GuestRangeValid(num_bytes_sent_ptr, 4, true)) {
    memory::store_and_swap<uint32_t>(GuestBytes(num_bytes_sent_ptr), uint32_t(ret));
  }

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

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, u32 addr_ptr) {
  if (!GuestRangeValid(addr_ptr, sizeof(XNADDR), true)) {
    return XnAddrStatus::XNET_GET_XNADDR_NONE;
  }
  // The PC's LAN address. Never "pending": CoD3 waits in a loop until the
  // address is ready (0x823C32E0).
  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup. (abEnet is never zero here.)
  xnet::GetTitleXnAddr(REX_KERNEL_MEMORY()->TranslateVirtual<XNADDR*>(addr_ptr));
  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, u32 addr_ptr) {
  if (GuestRangeValid(addr_ptr, sizeof(XNADDR), true)) {
    std::memset(GuestBytes(addr_ptr), 0, sizeof(XNADDR));
  }

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

u32 NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, u32 string_ptr, u32 string_size) {
  std::string text = rex::net::format_ipv4(in_addr);
  if (!string_size || !GuestRangeValid(string_ptr, string_size, true)) {
    return wsa::kEfault;
  }
  if (string_size < text.size() + 1) {
    GuestBytes(string_ptr)[0] = 0;
    return wsa::kEmsgsize;
  }
  std::memcpy(GuestBytes(string_ptr), text.c_str(), text.size() + 1);
  return 0;
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address). Between PCs it
// is the peer's own IPv4 address.
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, u32 xnaddr_ptr, u32 kid_ptr, u32 in_addr_ptr) {
  if (!GuestRangeValid(xnaddr_ptr, sizeof(XNADDR), false) ||
      !GuestRangeValid(kid_ptr, sizeof(X_XNKID), false) ||
      !GuestRangeValid(in_addr_ptr, 4, true)) {
    return wsa::kEfault;
  }
  XNADDR xnaddr;
  std::memcpy(&xnaddr, GuestBytes(xnaddr_ptr), sizeof(xnaddr));
  X_XNKID kid;
  std::memcpy(&kid, GuestBytes(kid_ptr), sizeof(kid));
  uint32_t ina = xnaddr.ina;
  if (!ina || !PeerAllowed(ina, "peer")) {
    return wsa::kEinval;
  }
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    AddPeer(state, ina, xnaddr, kid);
  }
  memory::store_and_swap<uint32_t>(GuestBytes(in_addr_ptr), ina);
  return 0;
}

// Does the reverse of the above: the XNADDR and key of the peer at an
// address. A peer this PC hasn't looked up yet (a player joining this host)
// gets the XNADDR every PC makes for that address and the newest key.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, u32 xnaddr_ptr, u32 kid_ptr) {
  if (!GuestRangeValid(xnaddr_ptr, sizeof(XNADDR), true) ||
      (kid_ptr && !GuestRangeValid(kid_ptr, sizeof(X_XNKID), true))) {
    return wsa::kEfault;
  }
  if (!in_addr || !PeerAllowed(in_addr, "peer")) {
    return wsa::kEinval;
  }
  XNADDR xnaddr;
  X_XNKID kid;
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    PeerEntry* peer = FindPeer(state, in_addr);
    if (peer) {
      peer->last_used = ++state.clock;
    } else {
      // The newest registered key is the session's; failing that, the
      // newest key made here.
      if (state.keys.empty() && !state.has_created_kid) {
        return wsa::kEinval;
      }
      const X_XNKID& newest = state.keys.empty() ? state.created_kid : state.keys.back().kid;
      peer = &AddPeer(state, in_addr, MakeXnAddr(in_addr), newest);
      REXKRNL_DEBUG("XNet: new peer {}", rex::net::format_ipv4(in_addr));
    }
    xnaddr = peer->xnaddr;
    kid = peer->kid;
  }
  std::memcpy(GuestBytes(xnaddr_ptr), &xnaddr, sizeof(xnaddr));
  if (kid_ptr) {
    std::memcpy(GuestBytes(kid_ptr), &kid, sizeof(kid));
  }
  return 0;
}

// TSADDR: a title server's IN_ADDR and service id. Without Xbox Live it is
// a plain address.
u32 NetDll_XNetTsAddrToInAddr_entry(u32 caller, u32 tsaddr_ptr, u32 service_id, u32 kid_ptr,
                                    u32 in_addr_ptr) {
  if (!GuestRangeValid(tsaddr_ptr, 8, false) || !GuestRangeValid(in_addr_ptr, 4, true)) {
    return wsa::kEfault;
  }
  uint32_t ina = memory::load_and_swap<uint32_t>(GuestBytes(tsaddr_ptr));
  if (!ina || !PeerAllowed(ina, "server")) {
    return wsa::kEinval;
  }
  memory::store_and_swap<uint32_t>(GuestBytes(in_addr_ptr), ina);
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  auto it = std::find_if(state.peers.begin(), state.peers.end(),
                         [&](const PeerEntry& peer) { return peer.ina == in_addr; });
  if (it == state.peers.end()) {
    return wsa::kEinval;
  }
  state.peers.erase(it);
  return 0;
}

// There is no key exchange between PCs: a known peer is connected at once.
u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  if (rex::net::ipv4_is_loopback(in_addr)) {
    return 0;
  }
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  PeerEntry* peer = FindPeer(state, in_addr);
  if (!peer) {
    return wsa::kEinval;
  }
  peer->last_used = ++state.clock;
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  if (rex::net::ipv4_is_loopback(in_addr)) {
    return kConnectStatusConnected;
  }
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  // A peer forgotten (unregistered or dropped from a full table) is "lost",
  // so the game closes its connection instead of waiting on it.
  return FindPeer(state, in_addr) ? kConnectStatusConnected : kConnectStatusLost;
}

u32 NetDll_XNetCreateKey_entry(u32 caller, u32 kid_ptr, u32 key_ptr) {
  if (!GuestRangeValid(kid_ptr, sizeof(X_XNKID), true) ||
      !GuestRangeValid(key_ptr, sizeof(X_XNKEY), true)) {
    return wsa::kEfault;
  }
  X_XNKID kid;
  X_XNKEY key;
  xnet::CreateKey(&kid, &key);
  std::memcpy(GuestBytes(kid_ptr), &kid, sizeof(kid));
  std::memcpy(GuestBytes(key_ptr), &key, sizeof(key));
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, u32 kid_ptr, u32 key_ptr) {
  if (!GuestRangeValid(kid_ptr, sizeof(X_XNKID), false) ||
      !GuestRangeValid(key_ptr, sizeof(X_XNKEY), false)) {
    return wsa::kEfault;
  }
  X_XNKID kid;
  X_XNKEY key;
  std::memcpy(&kid, GuestBytes(kid_ptr), sizeof(kid));
  std::memcpy(&key, GuestBytes(key_ptr), sizeof(key));
  return xnet::RegisterKey(kid, key);
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, u32 kid_ptr) {
  if (!GuestRangeValid(kid_ptr, sizeof(X_XNKID), false)) {
    return wsa::kEfault;
  }
  X_XNKID kid;
  std::memcpy(&kid, GuestBytes(kid_ptr), sizeof(kid));
  auto same_kid = [&](const X_XNKID& other) {
    return !std::memcmp(other.ab, kid.ab, sizeof(kid.ab));
  };
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.keys.erase(std::remove_if(state.keys.begin(), state.keys.end(),
                                  [&](const KeyEntry& entry) { return same_kid(entry.kid); }),
                   state.keys.end());
  // Peers reached through that key go with it, as on the console.
  state.peers.erase(std::remove_if(state.peers.begin(), state.peers.end(),
                                   [&](const PeerEntry& peer) { return same_kid(peer.kid); }),
                    state.peers.end());
  return 0;
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

// A link while the PC has an active network interface. The system link
// menus check this every second.
u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!CurrentTitle(state).lan) {
    return 0;
  }
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
         XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  // No name lookups: the port never contacts servers by name.
  if (pdns && GuestRangeValid(pdns.guest_address(), 4, true)) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
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
  if (pqos && GuestRangeValid(pqos.guest_address(), 4, true)) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = std::find(state.qos_blocks.begin(), state.qos_blocks.end(), qos.guest_address());
    if (it != state.qos_blocks.end()) {
      state.qos_blocks.erase(it);
    }
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

// The host's QoS listener answers probes with a data block. Between PCs
// there are no probes, so this only checks the arguments.
u32 NetDll_XNetQosListen_entry(u32 caller, u32 kid_ptr, u32 data_ptr, u32 data_size,
                               u32 bits_per_sec, u32 flags) {
  constexpr uint32_t kQosListenSetData = 0x04;
  if (!GuestRangeValid(kid_ptr, sizeof(X_XNKID), false)) {
    return wsa::kEfault;
  }
  if ((flags & kQosListenSetData) && data_size &&
      (data_size > 0x10000 || !GuestRangeValid(data_ptr, data_size, false))) {
    return wsa::kEfault;
  }
  return 0;
}

// QoS probes: every target is answered at once. A target system link may
// reach is "contacted" (CoD3 joins only contacted hosts); others are not.
// The result block isn't freed by CoD3 (it doesn't import XNetQosRelease),
// so the oldest blocks are freed once there are many.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 xnaddr_count, u32 xnaddrs_ptr, u32 kids_ptr,
                               u32 keys_ptr, u32 ina_count, u32 inas_ptr, u32 service_ids_ptr,
                               u32 probe_count, u32 bits_per_sec, u32 flags, u32 event_handle,
                               u32 result_ptr) {
  constexpr uint32_t kMaxTargets = 1024;
  if (xnaddr_count > kMaxTargets || ina_count > kMaxTargets ||
      !GuestRangeValid(result_ptr, 4, true) ||
      (xnaddr_count && !GuestRangeValid(xnaddrs_ptr, xnaddr_count * 4, false)) ||
      (ina_count && !GuestRangeValid(inas_ptr, ina_count * 4, false))) {
    return wsa::kEfault;
  }
  uint32_t count = xnaddr_count + ina_count;
  uint32_t size = 8 + std::max(count, 1u) * uint32_t(sizeof(XNQOSINFO));
  uint32_t block = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (!block) {
    return wsa::kEnobufs;
  }
  uint8_t* bytes = GuestBytes(block);
  std::memset(bytes, 0, size);
  auto qos = reinterpret_cast<XNQOS*>(bytes);
  qos->count = count;
  qos->count_pending = 0;
  auto infos = reinterpret_cast<XNQOSINFO*>(bytes + offsetof(XNQOS, info));
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t ina = 0;
    if (i < xnaddr_count) {
      uint32_t xnaddr_ptr = memory::load_and_swap<uint32_t>(GuestBytes(xnaddrs_ptr + i * 4));
      if (GuestRangeValid(xnaddr_ptr, sizeof(XNADDR), false)) {
        ina = memory::load_and_swap<uint32_t>(GuestBytes(xnaddr_ptr));
      }
    } else {
      ina = memory::load_and_swap<uint32_t>(GuestBytes(inas_ptr + (i - xnaddr_count) * 4));
    }
    XNQOSINFO& info = infos[i];
    info.flags = kQosComplete;
    if (ina && PeerAllowed(ina, "QoS probe to")) {
      uint16_t probes = static_cast<uint16_t>(std::clamp(probe_count, 1u, 0xFFFFu));
      info.flags = kQosComplete | kQosTargetContacted;
      info.probes_xmit = probes;
      info.probes_recv = probes;
      info.rtt_min_in_msecs = kQosRttMs;
      info.rtt_med_in_msecs = kQosRttMs;
      info.up_bits_per_sec = kQosBitsPerSec;
      info.down_bits_per_sec = kQosBitsPerSec;
    }
  }
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.qos_blocks.push_back(block);
    if (state.qos_blocks.size() > kMaxQosBlocks) {
      REX_KERNEL_MEMORY()->SystemHeapFree(state.qos_blocks.front());
      state.qos_blocks.pop_front();
    }
  }
  memory::store_and_swap<uint32_t>(GuestBytes(result_ptr), block);
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
  return 0;
}

u32 NetDll_inet_addr_entry(u32 addr_ptr) {
  // A dotted address is at most 15 characters; read no further.
  if (!addr_ptr) {
    return -1;
  }
  char text[16] = {};
  for (uint32_t i = 0; i < sizeof(text); ++i) {
    if (!GuestRangeValid(addr_ptr + i, 1, false)) {
      return -1;
    }
    text[i] = static_cast<char>(GuestBytes(addr_ptr + i)[0]);
    if (!text[i]) {
      break;
    }
  }
  if (text[15]) {
    return -1;
  }
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (!text[0]) {
    return 0;
  }
  uint32_t addr = 0;
  if (!rex::net::parse_ipv4(text, &addr)) {
    return -1;
  }
  // The result is an IN_ADDR: network order in guest memory.
  return addr;
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  uint32_t error =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (error) {
    socket->Release();
    SetError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  uint32_t error = 0;
  auto ret = socket->Shutdown(how, &error);
  if (ret == -1) {
    SetError(error);
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname, u32 optval_ptr,
                            u32 optlen) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  // Every option the host knows is 4 bytes; read no more than that.
  uint32_t read_length = std::min(optlen, 4u);
  if (read_length && !GuestRangeValid(optval_ptr, read_length, false)) {
    SetError(wsa::kEfault);
    return -1;
  }

  uint32_t error =
      socket->SetOption(level, optname, read_length ? GuestBytes(optval_ptr) : nullptr, optlen);
  if (error) {
    SetError(error);
    return -1;
  }
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, u32 arg_ptr) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!GuestRangeValid(arg_ptr, 4, true)) {
    SetError(wsa::kEfault);
    return -1;
  }

  // The argument is a u_long, big-endian in guest memory.
  uint32_t value = memory::load_and_swap<uint32_t>(GuestBytes(arg_ptr));
  uint32_t error = socket->IOControl(cmd, &value);
  if (error) {
    SetError(error);
    return -1;
  }
  memory::store_and_swap<uint32_t>(GuestBytes(arg_ptr), value);
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, u32 name_ptr, u32 namelen) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  // Bound exactly where the guest asks.
  SockAddrV4 name;
  uint32_t error = ReadGuestSockaddr(name_ptr, namelen, &name);
  if (!error) {
    error = socket->Bind(name);
  }
  if (error) {
    SetError(error);
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, u32 name_ptr, u32 namelen) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  SockAddrV4 name;
  uint32_t error = ReadGuestSockaddr(name_ptr, namelen, &name);
  if (!error && !PeerAllowed(name.addr, "connect to")) {
    error = wsa::kEhostunreach;
  }
  if (!error) {
    error = socket->Connect(name);
  }
  if (error) {
    SetError(error);
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  uint32_t error = socket->Listen(backlog);
  if (error) {
    SetError(error);
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, u32 addr_ptr, u32 addrlen_ptr) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  uint32_t error = CheckGuestSockaddrOut(addr_ptr, addrlen_ptr);
  if (error) {
    SetError(error);
    return -1;
  }

  SockAddrV4 from;
  auto new_socket = socket->Accept(&from, &error);
  if (!new_socket) {
    SetError(error);
    return -1;
  }
  if (!PeerAllowed(from.addr, "connection from")) {
    new_socket->Close();
    new_socket->Release();
    SetError(wsa::kEconnaborted);
    return -1;
  }
  WriteGuestSockaddr(addr_ptr, addrlen_ptr, from);
  return new_socket->handle();
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count = 0;
  object_ref<XSocket> sockets[64];

  // Returns 0 or a Winsock error code.
  uint32_t Load(const x_fd_set* guest_set) {
    uint32_t guest_count = guest_set->fd_count;
    if (guest_count > 64) {
      return wsa::kEinval;
    }
    this->count = 0;
    for (uint32_t i = 0; i < guest_count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        return wsa::kEnotsock;
      }
#if !REX_PLATFORM_WIN32
      // A POSIX fd_set only holds descriptors below FD_SETSIZE.
      if (socket->native_handle() >= FD_SETSIZE) {
        return wsa::kEinval;
      }
#endif
      this->sockets[this->count++] = socket;
    }
    return 0;
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set, int* max_fd) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
      *max_fd = std::max(*max_fd, static_cast<int>(this->sockets[i]->native_handle()));
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

i32 NetDll_select_entry(i32 caller, i32 nfds, u32 readfds_ptr, u32 writefds_ptr,
                        u32 exceptfds_ptr, u32 timeout_ptr) {
  const u32 set_ptrs[3] = {readfds_ptr, writefds_ptr, exceptfds_ptr};
  host_set host_sets[3];
  fd_set native_sets[3] = {};
  x_fd_set* guest_sets[3] = {};
  int max_fd = -1;
  for (int i = 0; i < 3; ++i) {
    if (!set_ptrs[i]) {
      continue;
    }
    if (!GuestRangeValid(set_ptrs[i], sizeof(x_fd_set), true)) {
      SetError(wsa::kEfault);
      return -1;
    }
    guest_sets[i] = REX_KERNEL_MEMORY()->TranslateVirtual<x_fd_set*>(set_ptrs[i]);
    uint32_t error = host_sets[i].Load(guest_sets[i]);
    if (error) {
      SetError(error);
      return -1;
    }
    host_sets[i].Store(&native_sets[i], &max_fd);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    if (!GuestRangeValid(timeout_ptr, 8, false)) {
      SetError(wsa::kEfault);
      return -1;
    }
    int32_t seconds = memory::load_and_swap<int32_t>(GuestBytes(timeout_ptr));
    int32_t microseconds = memory::load_and_swap<int32_t>(GuestBytes(timeout_ptr + 4));
    chrono::Clock::ScaleGuestDurationTimeval(&seconds, &microseconds);
    timeout = {};
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;
    timeout_in = &timeout;
  }
  // Winsock ignores nfds; POSIX needs the highest descriptor plus one.
  int ret = select(max_fd + 1, guest_sets[0] ? &native_sets[0] : nullptr,
                   guest_sets[1] ? &native_sets[1] : nullptr,
                   guest_sets[2] ? &native_sets[2] : nullptr, timeout_in);
  if (ret < 0) {
    SetError(rex::net::socket_last_error());
    return -1;
  }
  for (int i = 0; i < 3; ++i) {
    if (guest_sets[i]) {
      host_sets[i].UpdateFrom(&native_sets[i]);
      host_sets[i].Store(guest_sets[i]);
    }
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (buf_len && !GuestRangeValid(buf_ptr, buf_len, true)) {
    SetError(wsa::kEfault);
    return -1;
  }

  uint32_t error = 0;
  int ret = socket->Recv(buf_len ? GuestBytes(buf_ptr) : nullptr, buf_len, flags, &error);
  if (ret < 0) {
    SetError(error);
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags,
                          u32 from_ptr, u32 fromlen_ptr) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (buf_len && !GuestRangeValid(buf_ptr, buf_len, true)) {
    SetError(wsa::kEfault);
    return -1;
  }
  uint32_t error = CheckGuestSockaddrOut(from_ptr, fromlen_ptr);
  if (error) {
    SetError(error);
    return -1;
  }
  uint8_t* buf = buf_len ? GuestBytes(buf_ptr) : nullptr;
  bool datagram = socket->type() == XSocket::X_SOCK_DGRAM;

  // Datagrams from addresses system link may not use, and VDP datagrams
  // with a bad header, are dropped here; the next one is read instead.
  for (int dropped = 0;; ++dropped) {
    SockAddrV4 from;
    uint32_t full_len = 0;
    int ret = socket->RecvFrom(buf, buf_len, flags, &from, &error, &full_len);
    if (ret < 0 && error != wsa::kEmsgsize) {
      SetError(error);
      return -1;
    }
    bool drop = false;
    if (datagram) {
      if (!PeerAllowed(from.addr, "packet from")) {
        drop = true;
      } else if (socket->is_vdp() && buf_len >= 2 && full_len) {
        // The header is checked against the datagram's real length.
        drop = !VdpHeaderValid(buf, full_len);
      }
    }
    if (drop) {
      if (flags & wsa::kMsgPeek) {
        // A peeked datagram is still queued: read it to discard it.
        uint8_t scratch[2];
        uint32_t ignored = 0;
        socket->RecvFrom(scratch, sizeof(scratch), flags & ~wsa::kMsgPeek, nullptr, &ignored);
      }
      if (dropped >= 255) {
        // Don't spin on a flood; the guest polls again.
        SetError(wsa::kEwouldblock);
        return -1;
      }
      continue;
    }
    WriteGuestSockaddr(from_ptr, fromlen_ptr, from);
    if (ret < 0) {
      SetError(error);
      return -1;
    }
    return ret;
  }
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (buf_len && !GuestRangeValid(buf_ptr, buf_len, false)) {
    SetError(wsa::kEfault);
    return -1;
  }

  uint32_t error = 0;
  int ret = socket->Send(buf_len ? GuestBytes(buf_ptr) : nullptr, buf_len, flags, &error);
  if (ret < 0) {
    SetError(error);
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags,
                        u32 to_ptr, u32 to_len) {
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (buf_len && !GuestRangeValid(buf_ptr, buf_len, false)) {
    SetError(wsa::kEfault);
    return -1;
  }
  SockAddrV4 to;
  if (to_ptr) {
    uint32_t error = ReadGuestSockaddr(to_ptr, to_len, &to);
    if (error) {
      SetError(error);
      return -1;
    }
  }
  return SendDatagram(socket.get(), buf_len ? GuestBytes(buf_ptr) : nullptr, buf_len, flags,
                      to_ptr ? &to : nullptr);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, u32 fd_set_ptr) {
  if (!GuestRangeValid(fd_set_ptr, sizeof(x_fd_set), false)) {
    return 0;
  }
  auto fd_set = REX_KERNEL_MEMORY()->TranslateVirtual<const x_fd_set*>(fd_set_ptr);
  const uint32_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint32_t i = 0; i < max_fd_count; i++) {
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
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetTsAddrToInAddr, rex::kernel::xam::NetDll_XNetTsAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)

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
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
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
