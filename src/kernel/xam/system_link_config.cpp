#include "system_link_config.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <mutex>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

#include "../../core/live_invite.h"
#include "../../core/relay_invite.h"
#include "lan_system_link.h"

namespace rex::kernel::xam::system_link {
namespace {
std::shared_ptr<const Config> snapshot;
// -1 retains live-cvar behavior for hosts that do not initialize a snapshot.
std::atomic<int> frozen_mode{-1};
std::mutex initialization;
// Writers hold both locks; readers take only this one, which never nests the
// cvar registry mutex that status getters run under.
std::mutex snapshot_mutex;
std::mutex status_mutex;
std::string startup_failure;

std::string& ProfileName() {
  static std::string name;
  return name;
}
std::string& InviteCode() {
  static std::string code;
  return code;
}
std::string& LiveInviteCode() {
  static std::string code;
  return code;
}
bool& LanEnabled() {
  static bool enabled = false;
  return enabled;
}
bool& LiveSessions() {
  static bool enabled = false;
  return enabled;
}
bool& Direct() {
  static bool enabled = false;
  return enabled;
}
int32_t& DirectDropAfter() {
  static int32_t seconds = 0;
  return seconds;
}
// Trims ASCII whitespace and one pair of matching quotes from pasted text.
std::string_view TrimPasted(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n\f\v");
  value = first == std::string_view::npos
              ? std::string_view{}
              : value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')))
    value = value.substr(1, value.size() - 2);
  return value;
}
bool ParseBool(std::string_view value, bool& target) {
  if (value == "true" || value == "1" || value == "yes")
    target = true;
  else if (value == "false" || value == "0" || value == "no")
    target = false;
  else
    return false;
  return true;
}
using namespace rex::cvar;
FlagRegistrar profile_flag({.name = "system_link_profile_name",
                            .type = FlagType::String,
                            .category = "Online",
                            .description = "System Link player name (empty uses Player-XXXX)",
                            .setter =
                                [](std::string_view value) {
                                  if (!ValidProfileName(value))
                                    return false;
                                  ProfileName() = value;
                                  return true;
                                },
                            .getter = [] { return ProfileName(); },
                            .lifecycle = Lifecycle::kRequiresRestart,
                            .default_value = "",
                            .audience = Audience::kPlayer,
                            .persistence = Persistence::kConfig});
FlagRegistrar invite_flag(
    {.name = "system_link_invite",
     .type = FlagType::String,
     .category = "Online",
     .description = "Paste the relay host's invite code, save settings, then restart",
     .setter =
         [](std::string_view value) {
           const auto first = value.find_first_not_of(" \t\r\n\f\v");
           value = first == std::string_view::npos
                       ? std::string_view{}
                       : value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
           if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                     (value.front() == '\'' && value.back() == '\'')))
             value = value.substr(1, value.size() - 2);
           if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == ' ';
               }))
             return false;
           InviteCode() = value;
           return true;
         },
     .getter = [] { return InviteCode(); },
     .lifecycle = Lifecycle::kRequiresRestart,
     .default_value = "",
     .audience = Audience::kPlayer,
     .persistence = Persistence::kConfig});
FlagRegistrar live_invite_flag(
    {.name = "live_invite",
     .type = FlagType::String,
     .category = "Online",
     .description = "Join a Live session from an invite code, for this session only",
     .setter =
         [](std::string_view value) {
           value = TrimPasted(value);
           if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
               }))
             return false;
           LiveInviteCode() = value;
           return true;
         },
     .getter = [] { return LiveInviteCode(); },
     .lifecycle = Lifecycle::kRequiresRestart,
     .default_value = "",
     .audience = Audience::kAdvanced,
     .persistence = Persistence::kNeverPersist});
FlagRegistrar lan_flag({.name = "system_link_lan_enabled",
                        .type = FlagType::Boolean,
                        .category = "Online",
                        .description = "Enable direct LAN System Link (requires a local address)",
                        .setter =
                            [](std::string_view value) {
                              if (value == "true" || value == "1" || value == "yes")
                                LanEnabled() = true;
                              else if (value == "false" || value == "0" || value == "no")
                                LanEnabled() = false;
                              else
                                return false;
                              return true;
                            },
                        .getter = [] { return LanEnabled() ? "true" : "false"; },
                        .lifecycle = Lifecycle::kRequiresRestart,
                        .default_value = "false",
                        .audience = Audience::kPlayer,
                        .persistence = Persistence::kConfig});
FlagRegistrar live_sessions_flag(
    {.name = "live_sessions",
     .type = FlagType::Boolean,
     .category = "Online",
     .description = "List and join Live-mode sessions through the relay (relay mode only)",
     .setter = [](std::string_view value) { return ParseBool(value, LiveSessions()); },
     .getter = [] { return LiveSessions() ? "true" : "false"; },
     .lifecycle = Lifecycle::kRequiresRestart,
     .default_value = "false",
     .audience = Audience::kPlayer,
     .persistence = Persistence::kConfig});
FlagRegistrar direct_flag(
    {.name = "system_link_direct",
     .type = FlagType::Boolean,
     .category = "Online",
     .description = "Try direct UDP paths to other relay players, keeping the relay as fallback",
     .setter = [](std::string_view value) { return ParseBool(value, Direct()); },
     .getter = [] { return Direct() ? "true" : "false"; },
     .lifecycle = Lifecycle::kRequiresRestart,
     .default_value = "false",
     .audience = Audience::kPlayer,
     .persistence = Persistence::kConfig});
FlagRegistrar direct_drop_flag(
    {.name = "system_link_direct_drop_after",
     .type = FlagType::Int32,
     .category = "Online",
     .description = "Test only: drop received direct datagrams this many seconds into a direct "
                    "path (0 is off)",
     .setter =
         [](std::string_view value) {
           int32_t seconds = 0;
           const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
           if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
               seconds < 0 || seconds > 3600)
             return false;
           DirectDropAfter() = seconds;
           return true;
         },
     .getter = [] { return std::to_string(DirectDropAfter()); },
     .lifecycle = Lifecycle::kRequiresRestart,
     .default_value = "0",
     .audience = Audience::kDeveloper,
     .persistence = Persistence::kSessionOnly});
FlagRegistrar status_flag({.name = "system_link_status",
                           .type = FlagType::String,
                           .category = "Online",
                           .description = "System Link connection status for this session",
                           .setter = [](std::string_view) { return false; },
                           .getter = Status,
                           .lifecycle = Lifecycle::kInitOnly,
                           .default_value = "Off",
                           .audience = Audience::kPlayer,
                           .persistence = Persistence::kNeverPersist});

// An advertised address must encode into a Live invite, which is length limited;
// the other fields have fixed sizes, so any session and token give the same verdict.
bool ValidAdvertisedAddress(std::string_view address) {
  auto parsed = relay_link::ParseAddress(address);
  if (!parsed || !relay::ParseInviteAddress(address))
    return false;
  relay::LiveInvite invite;
  invite.session_id = ~uint64_t{0};
  invite.address = std::move(*parsed);
  return relay::EncodeLiveInvite(invite).has_value();
}

Config LiveConfig() {
  Config config;
  config.lan_enabled = Query<bool>("system_link_lan_enabled");
  config.address = GetFlagByName("system_link_relay_address");
  config.room = GetFlagByName("system_link_relay_room");
  config.secret_file = GetFlagByName("system_link_relay_secret_file");
  config.advertised_address = GetFlagByName("system_link_advertised_address");
  if (!ValidAdvertisedAddress(config.advertised_address))
    config.advertised_address.clear();
  config.mode = !config.address.empty() || !config.room.empty() || !config.secret_file.empty()
                    ? Mode::kRelay
                : config.lan_enabled ? Mode::kLan
                                     : Mode::kOff;
  config.local_address = GetFlagByName("system_link_local_address");
  config.xuid = Query<uint64_t>("system_link_xuid");
  config.profile_name = GetFlagByName("system_link_profile_name");
  config.live_sessions = Query<bool>("live_sessions");
  config.direct = Direct();
  config.direct_drop_after = static_cast<uint32_t>(DirectDropAfter());
  return config;
}
}  // namespace

bool ValidProfileName(std::string_view name) {
  if (name.size() > 15 || (!name.empty() && (name.front() == ' ' || name.back() == ' ')))
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == '-' || c == '_' || c == '.';
  });
}
std::string InviteRelayAddress(const Config& config) {
  return config.advertised_address.empty() ? config.address : config.advertised_address;
}
Config CurrentConfig() {
  std::shared_ptr<const Config> frozen;
  {
    std::lock_guard lock(snapshot_mutex);
    frozen = snapshot;
  }
  return frozen ? *frozen : LiveConfig();
}
Mode CurrentMode() {
  const auto mode = frozen_mode.load(std::memory_order_acquire);
  return mode < 0 ? LiveConfig().mode : static_cast<Mode>(mode);
}

bool LiveSessionsActive() {
  const auto config = CurrentConfig();
  return config.live_sessions && config.mode == Mode::kRelay && config.error.empty();
}

void FreezeConfig(const std::filesystem::path& config_path, RandomSource random) {
  std::lock_guard lock(initialization);
  if (snapshot)
    return;
  Config config = LiveConfig();
  if (config.advertised_address.empty() && !GetFlagByName("system_link_advertised_address").empty())
    REXLOG_WARN(
        "System Link: system_link_advertised_address is not a valid host:port, or is too long "
        "for a Live invite; ignoring it");
  // Invite text remains in the cvar and saved config; only decoded storage is wiped.
  const auto& code = InviteCode();
  // A Live invite wins for this session: its relay and join token replace the
  // System Link invite, explicit relay settings and LAN, whose saved values stay.
  const auto& live_code = LiveInviteCode();
  if (!live_code.empty()) {
    std::vector<std::string_view> ignored;
    if (!code.empty())
      ignored.push_back("system_link_invite");
    if (!config.address.empty())
      ignored.push_back("system_link_relay_address");
    if (!config.room.empty())
      ignored.push_back("system_link_relay_room");
    if (!config.secret_file.empty())
      ignored.push_back("system_link_relay_secret_file");
    if (config.lan_enabled)
      ignored.push_back("system_link_lan_enabled");
    if (!ignored.empty())
      REXLOG_INFO("System Link: live_invite overrides {} for this session",
                  fmt::join(ignored, ", "));
    config.mode = Mode::kRelay;
    config.live_sessions = true;
    config.lan_enabled = false;
    config.address.clear();
    config.room.clear();
    config.secret_file.clear();
    auto invite = relay::DecodeLiveInvite(live_code);
    if (!invite) {
      config.error = invite.error().message;
    } else {
      config.address = invite->address.host + ":" + std::to_string(invite->address.port);
      const auto secret = invite->token_secret.bytes();
      config.join_token = std::make_shared<const relay_link::JoinToken>(
          relay_link::JoinToken{invite->token_id,
                                relay_link::Secret(std::string_view(
                                    reinterpret_cast<const char*>(secret.data()), secret.size())),
                                0});
      config.startup_invite = invite->session_id;
    }
  } else if (!code.empty()) {
    const bool explicit_relay = config.mode == Mode::kRelay;
    config.mode = Mode::kRelay;
    if (explicit_relay) {
      config.error = "invite and explicit relay settings cannot be combined";
    } else {
      auto invite = relay::DecodeInvite(code);
      if (!invite) {
        config.error = invite.error().message;
      } else {
        config.address = invite->address.host + ":" + std::to_string(invite->address.port);
        config.room = std::move(invite->room);
        config.invite_secret = std::make_shared<relay_link::Secret>(invite->SecretView());
      }
    }
  }
  if (config.error.empty() && config.mode == Mode::kRelay) {
    if (config.lan_enabled)
      config.error = "LAN and relay cannot be enabled together";
    else if (!relay_link::ParseAddress(config.address) ||
             (!config.join_token && (config.room.empty() || config.room.size() > 32 ||
                                     (!config.invite_secret && config.secret_file.empty()))))
      config.error = "bad relay settings";
  }
  if (config.mode != Mode::kOff && !config.xuid) {
    auto seed = random();
    if (!seed) {
      if (config.error.empty())
        config.error = "could not create player identity";
    } else {
      uint64_t low = 0;
      for (size_t i = 0; i < 6; ++i)
        low |= uint64_t((*seed)[i]) << (i * 8);
      // Zero is excluded without an unbounded retry on a broken random provider.
      config.xuid = 0xB13E000000000000ULL | (low ? low : 1);
      assert((config.xuid & 0x00C0000000000000ULL) == 0);
      SetFlagByName("system_link_xuid", std::to_string(config.xuid));
      bool saved = false;
      try {
        SaveConfigSubset(config_path, {"system_link_xuid"});
        auto table = toml::parse_file(config_path.string());
        saved = table["system_link_xuid"].value<std::string>() == std::to_string(config.xuid);
      } catch (const std::exception&) {}
      if (!saved)
        REXLOG_WARN("System Link player identity could not be saved; using it for this session");
    }
  }
  if (config.mode != Mode::kOff && config.profile_name.empty())
    config.profile_name = fmt::format("Player-{:04X}", config.xuid & 0xFFFF);
  if (config.error.empty() && config.mode == Mode::kLan) {
    lan::LanSystemLinkState validation;
    if (!validation.Configure(config.local_address, config.xuid))
      config.error = "bad LAN settings";
  }
  auto frozen = std::make_shared<const Config>(std::move(config));
  std::lock_guard publish(snapshot_mutex);
  snapshot = std::move(frozen);
  frozen_mode.store(static_cast<int>(snapshot->mode), std::memory_order_release);
}
void Initialize(const std::filesystem::path& config_path) {
  FreezeConfig(config_path, relay::RandomSeed);
}
Result<relay_link::Secret> ReadSecret(const Config& config) {
  if (config.invite_secret)
    return relay_link::Secret(config.invite_secret->view());
  return relay_link::ReadSecret(config.secret_file);
}
std::optional<relay_link::JoinToken> CopyJoinToken(const Config& config) {
  if (!config.join_token)
    return std::nullopt;
  return relay_link::JoinToken{config.join_token->id,
                               relay_link::Secret(config.join_token->secret.view()),
                               config.join_token->expiry_seconds};
}
void SetStartupFailure(std::string reason) {
  std::lock_guard lock(status_mutex);
  startup_failure = std::move(reason);
}
std::string RelayStatus(const relay_link::Stats& stats) {
  switch (stats.state) {
    case relay_link::Association::kLive: {
      const auto* octets = reinterpret_cast<const uint8_t*>(&stats.ipv4);
      auto status = fmt::format("Relay: connected as {}.{}.{}.{}", octets[0], octets[1], octets[2],
                                octets[3]);
      if (stats.direct_attempted_peers)
        status += fmt::format(" (direct: {} of {} peers)", stats.direct_peers,
                              stats.direct_attempted_peers);
      return status;
    }
    case relay_link::Association::kLost:
      return "Relay: lost connection";
    case relay_link::Association::kFailed:
      return "Relay failed: " + stats.last_failure;
    case relay_link::Association::kJoining:
      return "Relay: connecting";
    case relay_link::Association::kStopped:
      if (!stats.last_failure.empty())
        return "Relay failed: " + stats.last_failure;
      return stats.started ? "Relay: disconnected" : "Relay: waiting for System Link";
  }
  return "Relay: disconnected";
}
std::string Status() {
  const auto config = CurrentConfig();
  if (!config.error.empty())
    return "Relay failed: " + config.error;
  const char* live_inactive = config.live_sessions ? " (Live sessions inactive)" : "";
  if (config.mode == Mode::kOff)
    return std::string("Off") + live_inactive;
  if (config.mode == Mode::kLan)
    return std::string("LAN") + live_inactive;
  {
    std::lock_guard lock(status_mutex);
    if (!startup_failure.empty())
      return "Relay failed: " + startup_failure;
  }
  auto& client = relay_link::SystemLinkClient();
  return RelayStatus(client.stats());
}
uint32_t RelayStartup::Start(const Config& config, relay_link::Client& client,
                             lan::LanSystemLinkState& lan) {
  auto disconnected = [&](std::string reason, std::string_view detail) {
    REXKRNL_ERROR("XNetStartup: {}; continuing without a network, relay references={}", detail,
                  references_);
    SetStartupFailure(reason);
    client.MarkFailed(std::move(reason));
    return 0u;
  };
  // A rejected config, such as a damaged invite, cannot start until the next launch.
  if (!config.error.empty()) {
    ++references_;
    return disconnected(config.error, config.error);
  }
  if (config.lan_enabled || !config.xuid) {
    REXKRNL_ERROR(
        "XNetStartup: relay requires a nonzero system_link_xuid and "
        "system_link_lan_enabled=false");
    return X_ERROR_INVALID_PARAMETER;
  }
  ++references_;
  if (client.stats().state == relay_link::Association::kLive) {
    REXKRNL_DEBUG("XNetStartup: relay references={}", references_);
    return 0;
  }
  auto server = relay_link::Resolve(config.address);
  auto token = CopyJoinToken(config);
  auto secret = token ? Result<relay_link::Secret>(relay_link::Secret()) : ReadSecret(config);
  auto fingerprint = relay_link::HostFingerprint();
  if (!server || !secret || !fingerprint) {
    const auto& error = !server ? server.error() : !secret ? secret.error() : fingerprint.error();
    return disconnected(!server   ? "relay address not resolved"
                        : !secret ? "bad relay settings"
                                  : "executable unreadable",
                        error.message);
  }
  SetStartupFailure({});
  relay_link::DirectOptions direct;
  direct.enabled = config.direct;
  direct.drop_after_seconds = config.direct_drop_after;
  client.SetDirect(direct);
  auto started = token ? client.Start(*server, std::move(*token), *fingerprint)
                       : client.Start(*server, config.room, std::move(*secret), *fingerprint);
  if (!started) {
    auto reason = client.stats().last_failure;
    // The relay rejects an unknown, expired or revoked token as a wrong secret.
    if (config.join_token && reason == "wrong secret")
      reason = kInviteExpired;
    return disconnected(reason.empty() ? started.error().message : std::move(reason),
                        started.error().message);
  }
  const auto ipv4 = client.ipv4();
  const auto* octets = reinterpret_cast<const uint8_t*>(&ipv4);
  const auto address = fmt::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
  if (lan.local_ipv4() != ipv4) {
    // Sessions and peer mappings refer to the old identity; relay ports live separately.
    lan.Reset();
    if (!lan.Configure(address, config.xuid)) {
      client.Stop();
      // MarkFailed then keeps guest sends failing rather than dropping.
      return disconnected("invalid relay identity", "invalid relay identity");
    }
  }
  REXKRNL_DEBUG("XNetStartup: relay virtual address={} references={}", address, references_);
  return 0;
}
std::optional<relay_link::Stats> RelayStartup::Cleanup(relay_link::Client& client) {
  if (references_ > 1) {
    --references_;
    return std::nullopt;
  }
  references_ = 0;
  return client.Stop();
}
uint32_t RelayTitleXnAddr(const relay_link::Client& client, const lan::LanSystemLinkState& lan,
                          lan::XnAddrSnapshot* address) {
  // One snapshot: Configure and Reset change the address and Ethernet bytes together.
  const auto local = lan.local_xnaddr();
  const auto state = client.stats().state;
  if (state == relay_link::Association::kLive && local.ipv4 && local.ipv4 == client.ipv4()) {
    *address = local;
    return kXnAddrStatic;
  }
  *address = {};
  // RakNet-derived titles seed from abEnet and fail on zero, so keep a link identity.
  if (local.ipv4)
    address->ethernet = local.ethernet;
  else
    address->ethernet.fill(0xCC);
  // PENDING invites titles to poll until acquisition ends, so report it only while
  // a handshake runs; NONE is terminal once nothing is acquiring an address.
  return state == relay_link::Association::kJoining ? kXnAddrPending : kXnAddrNone;
}
uint32_t QosListen(lan::LanSystemLinkState& lan, Mode mode, std::span<const uint8_t> payload,
                   uint32_t bits_per_second, uint32_t flags) {
  if (mode != Mode::kRelay && !lan.enabled())
    return X_ERROR_FUNCTION_FAILED;
  const auto result = lan.UpdateQos(payload, bits_per_second, flags);
  switch (result) {
    case lan::QosUpdateResult::kSuccess:
      REXKRNL_DEBUG("XNetQosListen: flags={:08X} bytes={} bits_per_second={}", flags,
                    payload.size(), bits_per_second);
      return 0;
    case lan::QosUpdateResult::kNotConfigured:
      // A relay that never joined has no identity; valid requests have no one to answer.
      if (mode == Mode::kRelay) {
        REXKRNL_DEBUG("XNetQosListen: relay disconnected flags={:08X}", flags);
        return 0;
      }
      // Direct LAN was reset between the mode check and the update.
      return X_ERROR_FUNCTION_FAILED;
    default:
      REXKRNL_ERROR("XNetQosListen: rejected flags={:08X} bytes={} result={}", flags,
                    payload.size(), static_cast<uint32_t>(result));
      return X_ERROR_INVALID_PARAMETER;
  }
}
namespace testing {
void ResetSnapshot() {
  std::lock_guard lock(initialization);
  std::lock_guard publish(snapshot_mutex);
  snapshot.reset();
  frozen_mode.store(-1, std::memory_order_release);
  SetStartupFailure({});
}
}  // namespace testing
}  // namespace rex::kernel::xam::system_link
