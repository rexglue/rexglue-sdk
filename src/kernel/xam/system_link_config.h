#pragma once

#include "lan_system_link.h"
#include "relay_system_link.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace rex::kernel::xam::system_link {

enum class Mode { kOff, kLan, kRelay };
struct Config {
  Mode mode = Mode::kOff;
  bool lan_enabled = false;
  std::string address;
  std::string room;
  std::string secret_file;
  // Valid system_link_advertised_address, or empty. Only invites sent to other
  // players use it; this instance always connects to `address`.
  std::string advertised_address;
  std::shared_ptr<const relay_link::Secret> invite_secret;
  // From live_invite: joins the token's room instead of a room and secret.
  std::shared_ptr<const relay_link::JoinToken> join_token;
  // Session named by live_invite, delivered once as an accepted invite.
  uint64_t startup_invite = 0;
  std::string local_address;
  uint64_t xuid = 0;
  std::string profile_name;
  // Live-mode sessions through the relay directory; takes effect only in relay mode.
  bool live_sessions = false;
  // Direct UDP paths between relay room members; takes effect only in relay mode.
  bool direct = false;
  // Developer fault injection, never persisted; see relay_link::DirectOptions.
  uint32_t direct_drop_after = 0;
  std::string error;
};
using RandomSource = std::function<Result<relay::Digest>()>;

bool ValidProfileName(std::string_view name);
// The relay address to tell other players: the advertised one when set.
std::string InviteRelayAddress(const Config& config);
Config CurrentConfig();
Mode CurrentMode();
// True when live_sessions is set in a valid relay configuration.
bool LiveSessionsActive();
// Called before guest threads start. Repeated calls retain the first snapshot.
void FreezeConfig(const std::filesystem::path& config_path, RandomSource random);
void Initialize(const std::filesystem::path& config_path);
std::string Status();
std::string RelayStatus(const relay_link::Stats& stats);
void SetStartupFailure(std::string reason);
Result<relay_link::Secret> ReadSecret(const Config& config);
// A fresh copy of the configured join token, or nullopt without one.
std::optional<relay_link::JoinToken> CopyJoinToken(const Config& config);
// Status reason for a token join the relay rejected.
inline constexpr std::string_view kInviteExpired = "invite expired; ask for a new invite";

// XNetStartup and XNetCleanup references in relay mode. Callers serialize.
class RelayStartup {
 public:
  // Return 0, or X_ERROR_INVALID_PARAMETER for LAN plus relay or zero XUID.
  // Config errors and unreachable relays succeed disconnected, like XNet on a no-link console.
  // Status gives the reason. Later startup retries the relay. Config errors last until restart.
  uint32_t Start(const Config& config, relay_link::Client& client, lan::LanSystemLinkState& lan);
  // XNetCleanup: releases one reference. The last one stops the client and
  // returns its final stats.
  std::optional<relay_link::Stats> Cleanup(relay_link::Client& client);
  uint32_t references() const { return references_; }

 private:
  uint32_t references_ = 0;
};

constexpr uint32_t kXnAddrPending = 0x00000000;
constexpr uint32_t kXnAddrNone = 0x00000001;
constexpr uint32_t kXnAddrStatic = 0x00000004;
// Relay-mode XNetGetTitleXnAddr: the virtual address while the association is
// live, pending while a handshake runs, and none otherwise, with no IPv4 address.
uint32_t RelayTitleXnAddr(const relay_link::Client& client, const lan::LanSystemLinkState& lan,
                          lan::XnAddrSnapshot* address);
// XNetQosListen after pointer validation: returns 0, X_ERROR_INVALID_PARAMETER,
// or X_ERROR_FUNCTION_FAILED when System Link is off or LAN is unconfigured.
// Relay mode accepts valid requests before it has an identity.
uint32_t QosListen(lan::LanSystemLinkState& lan, Mode mode, std::span<const uint8_t> payload,
                   uint32_t bits_per_second, uint32_t flags);

namespace testing {
void ResetSnapshot();
}
}  // namespace rex::kernel::xam::system_link
