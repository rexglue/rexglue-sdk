#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>
#include <spdlog/sinks/ostream_sink.h>
#include <toml++/toml.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xsocket.h>
#include <rex/system/xtypes.h>
#include "core/relay_invite.h"
#include "core/relay_transport.h"
#include "kernel/xam/system_link_config.h"

namespace rex::kernel::xam {
bool RelayRequested();
uint32_t XamGetSystemVersion_entry();
}  // namespace rex::kernel::xam
using namespace rex::kernel::xam;
using namespace rex::kernel::xam::system_link;
namespace {
struct StateFixture {
  StateFixture() {
    testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
  }
  ~StateFixture() {
    testing::ResetSnapshot();
    rex::cvar::testing::ResetAllForTesting();
  }
  void Set(std::string_view key, std::string_view value) {
    REQUIRE(rex::cvar::SetFlagByName(key, value));
  }
};

struct Fixture : StateFixture {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("system-link-config-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path path = directory / "fixture";
  Fixture() {
    REQUIRE(std::filesystem::create_directory(directory));
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(directory, error);
  }
  void Lan() {
    Set("system_link_lan_enabled", "true");
    Set("system_link_local_address", "127.0.0.1");
  }
  std::string Read() {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void Freeze() {
    FreezeConfig(path, [] { return rex::relay::Digest{0xCD, 0xAB, 2, 3, 4, 5}; });
  }
};
}  // namespace

TEST_CASE("system_link profile setter validates every startup source", "[system_link]") {
  StateFixture f;
  for (std::string_view name : {"", "A", "Player-1234", "A b_9.X", "123456789012345"}) {
    CHECK(ValidProfileName(name));
    f.Set("system_link_profile_name", name);
  }
  for (std::string name :
       {" leading", "trailing ", "two\nlines", "a/b", "1234567890123456", "\xc3\xa9", "a\tb"}) {
    CHECK_FALSE(ValidProfileName(name));
    CHECK_FALSE(rex::cvar::GetFlagInfo("system_link_profile_name")->setter(name));
    CHECK_FALSE(rex::cvar::SetFlagByName("system_link_profile_name", name));
    CHECK(rex::cvar::GetFlagByName("system_link_profile_name") == "123456789012345");
  }
}

TEST_CASE("system_link settings expose player advanced and read-only policies", "[system_link]") {
  StateFixture f;
  for (const char* key :
       {"system_link_invite", "system_link_profile_name", "system_link_lan_enabled",
        "system_link_xuid", "system_link_local_address", "system_link_relay_address",
        "system_link_relay_room", "system_link_relay_secret_file",
        "system_link_advertised_address"}) {
    const auto* flag = rex::cvar::GetFlagInfo(key);
    REQUIRE(flag);
    CHECK(flag->category == "Online");
    CHECK(flag->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
    CHECK(flag->persistence == rex::cvar::Persistence::kConfig);
    bool player = std::string_view(key) == "system_link_invite" ||
                  std::string_view(key) == "system_link_profile_name" ||
                  std::string_view(key) == "system_link_lan_enabled";
    CHECK(flag->audience ==
          (player ? rex::cvar::Audience::kPlayer : rex::cvar::Audience::kAdvanced));
  }
  auto* flag = rex::cvar::GetFlagInfo("system_link_status");
  REQUIRE(flag);
  CHECK(flag->lifecycle == rex::cvar::Lifecycle::kInitOnly);
  CHECK(flag->audience == rex::cvar::Audience::kPlayer);
  CHECK(flag->persistence == rex::cvar::Persistence::kNeverPersist);
  CHECK_FALSE(flag->setter("LAN"));
  CHECK_FALSE(rex::cvar::SetFlagByName("system_link_status", "LAN"));
  CHECK(flag->getter() == "Off");
}

TEST_CASE("system_link invite help names save and restart", "[system_link]") {
  const auto* flag = rex::cvar::GetFlagInfo("system_link_invite");
  REQUIRE(flag);
  CHECK(flag->description == "Paste the relay host's invite code, save settings, then restart");
}

TEST_CASE("system_link generated XUID persists and preserves unrelated config", "[system_link]") {
  Fixture f;
  {
    std::ofstream stream(f.path);
    stream << "# keep this\nunrelated = 17 # keep too\n[other]\nsystem_link_xuid = 7\n";
  }
  f.Lan();
  f.Freeze();
  auto config = CurrentConfig();
  CHECK(config.error.empty());
  CHECK(config.xuid == 0xB13E05040302ABCDULL);
  CHECK((config.xuid & 0x00C0000000000000ULL) == 0);
  CHECK((config.xuid & 0x0000FFFFFFFFFFFFULL) != 0);
  CHECK((config.xuid >> 48) == 0xB13E);
  CHECK(config.profile_name == "Player-ABCD");
  CHECK(f.Read().find("# keep this\nunrelated = 17 # keep too\n") == 0);
  CHECK(f.Read().find("system_link_xuid = \"" + std::to_string(config.xuid) + "\"") !=
        std::string::npos);
  CHECK(f.Read().find("system_link_lan_enabled") == std::string::npos);
  CHECK(f.Read().find("[other]\nsystem_link_xuid = 7\n") != std::string::npos);
  CHECK(Status() == "LAN");
  rex::system::xam::UserProfile profile;
  CHECK(profile.xuid() == config.xuid);
  CHECK(profile.name() == config.profile_name);
  testing::ResetSnapshot();
  rex::cvar::testing::ResetAllForTesting();
  rex::cvar::LoadConfig(f.path);
  CHECK(rex::cvar::Query<uint64_t>("system_link_xuid") == config.xuid);
}

TEST_CASE("system_link zero random identity remains nonzero", "[system_link]") {
  Fixture f;
  f.Lan();
  FreezeConfig(f.path, [] { return rex::relay::Digest{}; });
  CHECK(CurrentConfig().xuid == 0xB13E000000000001ULL);
}

TEST_CASE("system_link does not generate or write configured and disabled identities",
          "[system_link]") {
  Fixture f;
  SECTION("off") {}
  SECTION("configured") {
    f.Lan();
    f.Set("system_link_xuid", "42");
  }
  SECTION("command line") {
    f.Lan();
    char program[] = "test";
    char option[] = "--system_link_xuid=43";
    char* args[] = {program, option};
    rex::cvar::Init(2, args);
    REQUIRE(rex::cvar::GetFlagSource("system_link_xuid") == rex::cvar::Source::kCommandLine);
  }
  {
    std::ofstream stream(f.path);
    stream << "unrelated = 17\n";
  }
  const auto xuid = rex::cvar::Query<uint64_t>("system_link_xuid");
  FreezeConfig(f.path, []() -> rex::Result<rex::relay::Digest> {
    FAIL("randomness must not be requested");
    return rex::relay::Digest{};
  });
  CHECK(CurrentConfig().xuid == xuid);
  CHECK(f.Read() == "unrelated = 17\n");
}

TEST_CASE("system_link failed identity persistence retains the session identity", "[system_link]") {
  Fixture f;
  f.Lan();
  {
    std::ofstream stream(f.path);
    stream << "not a directory";
  }
  FreezeConfig(f.path / "config.toml", [] { return rex::relay::Digest{1}; });
  CHECK(CurrentConfig().xuid == 0xB13E000000000001ULL);
  CHECK(CurrentConfig().error.empty());
  CHECK(f.Read() == "not a directory");
}

TEST_CASE("system_link snapshot freezes invites identity and mode", "[system_link][invite]") {
  Fixture f;
  auto invite =
      rex::relay::EncodeInvite({"relay.example.net", 30100}, "testers", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  f.Freeze();
  const auto initial = CurrentConfig();
  REQUIRE(initial.error.empty());
  CHECK(initial.address == "relay.example.net:30100");
  CHECK(initial.room == "testers");
  REQUIRE(initial.invite_secret);
  CHECK(initial.invite_secret->view() == "0123456789ABCDEF");
  auto secret = ReadSecret(initial);
  REQUIRE(secret);
  CHECK(secret->view() == initial.invite_secret->view());
  secret->Clear();
  CHECK(initial.invite_secret->view() == "0123456789ABCDEF");
  f.Set("system_link_invite", "");
  f.Set("system_link_lan_enabled", "true");
  f.Set("system_link_profile_name", "Changed");
  f.Set("system_link_xuid", "9");
  f.Set("system_link_relay_address", "different:1");
  f.Freeze();
  CHECK(CurrentConfig().address == initial.address);
  CHECK(CurrentConfig().xuid == initial.xuid);
  CHECK(CurrentConfig().profile_name == initial.profile_name);
  CHECK(RelayRequested());
  CHECK(XamGetSystemVersion_entry() == 0x20449700);
  CHECK(Status() == "Relay: waiting for System Link");
  rex::system::xam::UserProfile profile;
  CHECK(profile.name() == initial.profile_name);
  CHECK(profile.xuid() == initial.xuid);
}

TEST_CASE("system_link snapshot refuses conflicting and incomplete settings",
          "[system_link][invite]") {
  Fixture f;
  auto invite = rex::relay::EncodeInvite({"a", 1}, "r", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  SECTION("explicit address") {
    f.Set("system_link_relay_address", "a:1");
  }
  SECTION("explicit room") {
    f.Set("system_link_relay_room", "r");
  }
  SECTION("explicit file") {
    f.Set("system_link_relay_secret_file", "secret");
  }
  SECTION("LAN with invite") {
    f.Lan();
  }
  SECTION("LAN with explicit relay") {
    f.Set("system_link_invite", "");
    f.Lan();
    f.Set("system_link_relay_address", "a:1");
  }
  SECTION("incomplete explicit settings") {
    f.Set("system_link_invite", "");
    f.Set("system_link_relay_address", "a:1");
  }
  SECTION("bad invite") {
    f.Set("system_link_invite", "RXR1-00");
  }
  f.Freeze();
  CHECK_FALSE(CurrentConfig().error.empty());
  CHECK(Status().starts_with("Relay failed: "));
  CHECK(Status().find(*invite) == std::string::npos);
}

TEST_CASE("system_link config error starts disconnected", "[system_link][invite]") {
  Fixture f;
  f.Set("system_link_invite", "True");
  f.Freeze();
  const auto config = CurrentConfig();
  CHECK(config.error == "not an invite code");
  CHECK(RelayRequested());
  relay_link::Client client;
  lan::LanSystemLinkState lan;
  RelayStartup startup;
  // A title may not survive a failed XNetStartup, so each attempt succeeds offline.
  for (uint32_t references = 1; references <= 2; ++references) {
    CHECK(startup.Start(config, client, lan) == 0);
    CHECK(startup.references() == references);
    CHECK(client.stats().state == relay_link::Association::kFailed);
    CHECK(RelayStatus(client.stats()) == "Relay failed: not an invite code");
  }
  CHECK(Status() == "Relay failed: not an invite code");
  CHECK_FALSE(lan.enabled());
  lan::XnAddrSnapshot address{};
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrNone);
  CHECK(address.ipv4 == 0);
  CHECK_FALSE(startup.Cleanup(client));
  CHECK(startup.Cleanup(client));
  CHECK(startup.references() == 0);
}

TEST_CASE("system_link off snapshot ignores later settings", "[system_link]") {
  Fixture f;
  f.Freeze();
  f.Lan();
  f.Set("system_link_relay_address", "a:1");
  CHECK(Status() == "Off");
  CHECK_FALSE(RelayRequested());
  CHECK(XamGetSystemVersion_entry() == 0);
  CHECK(rex::system::xam::UserProfile().name() == "User");
}

TEST_CASE("system_link direct options freeze with the online snapshot", "[system_link][direct]") {
  Fixture f;
  const auto* direct = rex::cvar::GetFlagInfo("system_link_direct");
  REQUIRE(direct);
  CHECK(direct->category == "Online");
  CHECK(direct->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(direct->audience == rex::cvar::Audience::kPlayer);
  CHECK(direct->persistence == rex::cvar::Persistence::kConfig);
  CHECK(direct->default_value == "false");
  const auto* drop = rex::cvar::GetFlagInfo("system_link_direct_drop_after");
  REQUIRE(drop);
  CHECK(drop->category == "Online");
  CHECK(drop->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(drop->audience == rex::cvar::Audience::kDeveloper);
  CHECK(drop->persistence == rex::cvar::Persistence::kSessionOnly);
  CHECK(drop->default_value == "0");
  for (const char* value : {"-1", "3601", "x", "1.5", ""})
    CHECK_FALSE(rex::cvar::SetFlagByName("system_link_direct_drop_after", value));
  CHECK_FALSE(rex::cvar::SetFlagByName("system_link_direct", "maybe"));

  auto invite = rex::relay::EncodeInvite({"a", 1}, "r", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  f.Set("system_link_direct", "true");
  f.Set("system_link_direct_drop_after", "5");
  f.Freeze();
  f.Set("system_link_direct", "false");
  f.Set("system_link_direct_drop_after", "0");
  f.Freeze();
  CHECK(CurrentConfig().direct);
  CHECK(CurrentConfig().direct_drop_after == 5);
  CHECK(rex::cvar::GetFlagByName("system_link_direct") == "false");
}

TEST_CASE("system_link relay status counts direct peers", "[system_link][direct]") {
  relay_link::Stats stats;
  stats.state = relay_link::Association::kLive;
  const std::array<uint8_t, 4> address{198, 18, 0, 2};
  std::memcpy(&stats.ipv4, address.data(), address.size());
  CHECK(RelayStatus(stats) == "Relay: connected as 198.18.0.2");
  stats.direct_attempted_peers = 1;
  stats.direct_peers = 1;
  CHECK(RelayStatus(stats) == "Relay: connected as 198.18.0.2 (direct: 1 of 1 peers)");
  stats.direct_attempted_peers = 3;
  stats.direct_peers = 0;
  CHECK(RelayStatus(stats) == "Relay: connected as 198.18.0.2 (direct: 0 of 3 peers)");
  stats.state = relay_link::Association::kLost;
  CHECK(RelayStatus(stats) == "Relay: lost connection");
}

TEST_CASE("system_link startup failures have plain status reasons", "[system_link]") {
  Fixture f;
  auto invite = rex::relay::EncodeInvite({"a", 1}, "r", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  f.Freeze();
  for (const char* reason :
       {"relay address not resolved", "executable unreadable", "bad relay settings"}) {
    SetStartupFailure(reason);
    CHECK(Status() == std::string("Relay failed: ") + reason);
  }
}

TEST_CASE("system_link invite setter normalizes pasted credentials", "[system_link][invite]") {
  Fixture f;
  for (const auto text : {" \t\r\n\f\vRXR1-AB CD \t\r\n\f\v", "\"RXR1-AB CD\"", "'RXR1-AB CD'"}) {
    f.Set("system_link_invite", text);
    CHECK(rex::cvar::GetFlagByName("system_link_invite") == "RXR1-AB CD");
  }
  for (const auto text : {"RXR1-AB\tCD", "RXR1-AB\nCD", "RXR1-AB\\CD", "RXR1-AB\"CD", "RXR1-AB/CD",
                          "\xc3\xa9", "''RXR1-AB''"}) {
    CHECK_FALSE(rex::cvar::SetFlagByName("system_link_invite", text));
    CHECK(rex::cvar::GetFlagByName("system_link_invite") == "RXR1-AB CD");
  }
  f.Set("system_link_invite", " \t''\r\n");
  CHECK(rex::cvar::GetFlagByName("system_link_invite").empty());
}

TEST_CASE("system_link explicit hosts retain resolver syntax", "[system_link][relay]") {
  Fixture f;
  f.Set("system_link_relay_address", "relay_test.example:30100");
  f.Set("system_link_relay_room", "testers");
  f.Set("system_link_relay_secret_file", "secret");
  f.Freeze();
  CHECK(CurrentConfig().error.empty());
  CHECK(relay_link::ParseAddress("relay_test.example:30100"));
  CHECK_FALSE(rex::relay::ParseInviteAddress("relay_test.example:30100"));
}

TEST_CASE("system_link advertised address replaces only the address given to other players",
          "[system_link][relay]") {
  Fixture f;
  rex::InitLoggingEarly();
  auto logger = rex::GetLogger(rex::log::core());
  REQUIRE(logger);
  std::ostringstream captured;
  logger->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
  struct Restore {
    std::shared_ptr<spdlog::logger> logger;
    ~Restore() { logger->sinks().pop_back(); }
  } restore{logger};
  f.Set("system_link_relay_address", "127.0.0.1:30100");
  f.Set("system_link_relay_room", "testers");
  f.Set("system_link_relay_secret_file", "secret");
  const auto* flag = rex::cvar::GetFlagInfo("system_link_advertised_address");
  REQUIRE(flag);
  CHECK(flag->default_value.empty());
  bool warned = false;
  std::string advertised;
  SECTION("unset") {}
  SECTION("valid hostname") {
    advertised = "play.example.net:40100";
  }
  SECTION("valid IPv4") {
    advertised = "203.0.113.7:30100";
  }
  SECTION("missing port") {
    advertised = "play.example.net";
    warned = true;
  }
  SECTION("bad port") {
    advertised = "play.example.net:70000";
    warned = true;
  }
  // The invite limit leaves 35 bytes for host:port; ":30100" takes six.
  SECTION("longest host that fits an invite") {
    advertised = std::string(29, 'h') + ":30100";
  }
  SECTION("shortest host that does not fit an invite") {
    advertised = std::string(30, 'h') + ":30100";
    warned = true;
  }
  SECTION("host that cannot be put in an invite") {
    advertised = "play_test.example:30100";
    warned = true;
  }
  f.Set("system_link_advertised_address", advertised);
  f.Freeze();
  const auto config = CurrentConfig();
  CHECK(config.error.empty());
  CHECK(config.address == "127.0.0.1:30100");
  const bool used = !advertised.empty() && !warned;
  CHECK(config.advertised_address == (used ? advertised : ""));
  CHECK(InviteRelayAddress(config) == (used ? advertised : "127.0.0.1:30100"));
  const auto logged = captured.str();
  const auto line = logged.find("system_link_advertised_address is not a valid");
  CHECK((line != std::string::npos) == warned);
  if (warned)
    CHECK(logged.find("system_link_advertised_address is not a valid", line + 1) ==
          std::string::npos);
  if (!advertised.empty())
    CHECK(logged.find(advertised) == std::string::npos);
}

TEST_CASE("system_link subset inserts identity after root arrays before tables",
          "[system_link][cvar]") {
  Fixture f;
  const std::string root = R"(array = [
  ["[", "]", "escaped\"[#"], # ] [
  ['[', ']'],
]
multiline = """
[not a table]
"""
)";
  const std::string table = "[other]\nvalue = 42\n";
  {
    std::ofstream stream(f.path);
    stream << root << table;
  }
  f.Set("system_link_xuid", "12771645593269305345");
  rex::cvar::SaveConfigSubset(f.path, {"system_link_xuid"});
  const auto saved = f.Read();
  CHECK(saved == root + "system_link_xuid = \"12771645593269305345\"\n" + table);
  const auto parsed = toml::parse_file(f.path.string());
  CHECK(parsed["system_link_xuid"].value<std::string>() == "12771645593269305345");
  CHECK(parsed["array"].as_array()->size() == 2);
}

TEST_CASE("system_link config strings round trip quotes and backslashes", "[system_link][cvar]") {
  Fixture f;
  const std::string value = "path\\with\"quotes\"\n\t\b\f";
  f.Set("system_link_relay_secret_file", value);
  SECTION("full save") {
    rex::cvar::SaveConfig(f.path);
  }
  SECTION("category serialization") {
    std::ofstream stream(f.path);
    stream << rex::cvar::SerializeToTOML("Online");
  }
  rex::cvar::testing::ResetAllForTesting();
  rex::cvar::LoadConfig(f.path);
  CHECK(rex::cvar::GetFlagByName("system_link_relay_secret_file") == value);
}

TEST_CASE("cvar parse errors keep descriptions without source excerpts", "[cvar][system_link]") {
  Fixture f;
  rex::InitLoggingEarly();
  auto logger = rex::GetLogger(rex::log::core());
  REQUIRE(logger);
  std::ostringstream captured;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
  logger->sinks().push_back(sink);
  struct Restore {
    std::shared_ptr<spdlog::logger> logger;
    ~Restore() { logger->sinks().pop_back(); }
  } restore{logger};
  SECTION("ordinary description") {
    {
      std::ofstream stream(f.path);
      stream << "value = [";
    }
    rex::cvar::LoadConfig(f.path);
    CHECK(captured.str().find("encountered end-of-file") != std::string::npos);
  }
  SECTION("source token") {
    {
      std::ofstream stream(f.path);
      stream << "value = trPRIVATE";
    }
    rex::cvar::LoadConfig(f.path);
    CHECK(captured.str().find("expected 'true', saw <source text redacted>") != std::string::npos);
    CHECK(captured.str().find("trP") == std::string::npos);
  }
  SECTION("duplicate key") {
    {
      std::ofstream stream(f.path);
      stream << "PRIVATE = 1\nPRIVATE = 2\n";
    }
    rex::cvar::LoadConfig(f.path);
    CHECK(captured.str().find("cannot redefine existing integer <source text redacted>") !=
          std::string::npos);
    CHECK(captured.str().find("PRIVATE") == std::string::npos);
  }
}

TEST_CASE("system_link unreachable relay startup succeeds without a network",
          "[system_link][relay][network]") {
  using rex::X_RESULT;
  using rex::X_STATUS;
  using namespace std::chrono_literals;
  Fixture f;
  auto invite = rex::relay::EncodeInvite({"a", 1}, "r", "0123456789ABCDEF");
  REQUIRE(invite);
  f.Set("system_link_invite", *invite);
  f.Freeze();
  // Holds Winsock for the guest socket below; the client releases its own reference.
  auto winsock = rex::relay::UdpSocket::Open("127.0.0.1", 0);
  if (!winsock)
    SKIP("Loopback UDP socket unavailable");
  uint16_t port = 0;
  {
    // Nothing listens on this port once the probe closes.
    auto probe = rex::relay::UdpSocket::Open("127.0.0.1", 0);
    if (!probe)
      SKIP("Loopback UDP socket unavailable");
    port = (*probe)->endpoint().port;
  }
  Config config;
  config.mode = Mode::kRelay;
  config.address = "127.0.0.1:" + std::to_string(port);
  config.room = "test";
  config.invite_secret = std::make_shared<relay_link::Secret>("secret");
  config.xuid = 0xB13E000000000001ULL;
  relay_link::Client client;
  lan::LanSystemLinkState lan;
  RelayStartup startup;

  config.xuid = 0;
  CHECK(startup.Start(config, client, lan) == X_ERROR_INVALID_PARAMETER);
  config.xuid = 0xB13E000000000001ULL;
  config.lan_enabled = true;
  CHECK(startup.Start(config, client, lan) == X_ERROR_INVALID_PARAMETER);
  config.lan_enabled = false;
  CHECK(startup.references() == 0);

  lan::XnAddrSnapshot address{};
  address.ipv4 = 0x01020304;
  const std::array<uint8_t, 6> fallback{0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrNone);
  CHECK(address.ipv4 == 0);
  CHECK(address.ethernet == fallback);

  rex::system::XSocket socket(nullptr);
  REQUIRE(socket.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                            rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);
  const rex::relay::Bytes payload{1, 2, 3};
  auto network_down = [&] {
    const auto bound = client.Bind(socket.native_handle(), 1000);
    REQUIRE_FALSE(bound);
    CHECK(bound.error() == relay_link::kNetworkDown);
    const auto sent = client.Send(socket.native_handle(), {0xFFFFFFFFu, 1000}, payload);
    REQUIRE_FALSE(sent);
    CHECK(sent.error() == relay_link::kNetworkDown);
  };

  // A failure before the handshake still fails guest I/O instead of dropping it.
  auto unreadable = config;
  unreadable.invite_secret.reset();
  unreadable.secret_file = (f.directory / "missing-secret").string();
  CHECK(startup.Start(unreadable, client, lan) == 0);
  CHECK(startup.references() == 1);
  CHECK(client.stats().state == relay_link::Association::kFailed);
  CHECK(Status() == "Relay failed: bad relay settings");
  CHECK(RelayStatus(client.stats()) == "Relay failed: bad relay settings");
  network_down();
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrNone);

  auto attempt = std::async(std::launch::async, [&] { return startup.Start(config, client, lan); });
  const auto joining_deadline = std::chrono::steady_clock::now() + 3s;
  while (client.stats().state != relay_link::Association::kJoining &&
         std::chrono::steady_clock::now() < joining_deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrPending);
  CHECK(address.ipv4 == 0);
  CHECK(address.ethernet == fallback);
  CHECK(attempt.get() == 0);
  CHECK(startup.references() == 2);
  CHECK(client.stats().state == relay_link::Association::kFailed);
  CHECK(Status() == "Relay failed: relay not reachable (handshake timeout)");
  CHECK_FALSE(lan.enabled());
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrNone);
  CHECK(address.ipv4 == 0);
  CHECK(address.online_ipv4 == 0);
  CHECK(address.ethernet == fallback);
  CHECK(QosListen(lan, Mode::kRelay, {}, 0, lan::kQosListenEnable) == 0);
  CHECK(QosListen(lan, Mode::kRelay, {}, 0, 0x100) == X_ERROR_INVALID_PARAMETER);
  CHECK(QosListen(lan, Mode::kLan, {}, 0, lan::kQosListenEnable) == X_ERROR_FUNCTION_FAILED);
  CHECK(QosListen(lan, Mode::kOff, {}, 0, 0x100) == X_ERROR_FUNCTION_FAILED);
  uint32_t converted = 0;
  CHECK_FALSE(lan.XnAddrToInAddr({.ipv4 = 0x01020304}, &converted));
  network_down();
  std::array<uint8_t, 16> buffer{};
  rex::relay::Endpoint source{};
  const auto received = client.Receive(socket.native_handle(), buffer, 0, source);
  REQUIRE_FALSE(received);
  CHECK(received.error() == 10022);

  auto server = rex::relay::UdpSocket::Open("127.0.0.1", port);
  if (!server)
    SKIP("Relay port was reused before the retry");
  rex::relay::Core core({}, "secret", rex::relay::Digest{42});
  rex::relay::TransportCounters counters;
  std::jthread pump([&](std::stop_token stop) {
    const auto start = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start);
      rex::relay::Pump(**server, core, now.count(), counters, 10);
    }
  });
  CHECK(startup.Start(config, client, lan) == 0);
  CHECK(startup.references() == 3);
  REQUIRE(client.stats().state == relay_link::Association::kLive);
  CHECK_FALSE(Status().starts_with("Relay failed: "));
  CHECK(RelayStatus(client.stats()) == "Relay: connected as 198.18.0.1");
  CHECK(lan.enabled());
  CHECK(lan.local_ipv4() == client.ipv4());
  CHECK(RelayTitleXnAddr(client, lan, &address) == kXnAddrStatic);
  CHECK(address.ipv4 == client.ipv4());
  CHECK(address.ethernet != fallback);
  const std::array<uint8_t, 2> qos_data{7, 8};
  CHECK(QosListen(lan, Mode::kRelay, qos_data, 0,
                  lan::kQosListenEnable | lan::kQosListenSetData) == 0);
  CHECK(lan.qos_snapshot().enabled);
  CHECK(lan.qos_snapshot().payload.size() == 2);
  const auto sent = client.Send(socket.native_handle(), {0xFFFFFFFFu, 1000}, payload);
  INFO("send error " << (sent ? 0 : sent.error()));
  CHECK(sent == 3);
  CHECK(startup.Start(config, client, lan) == 0);
  CHECK(startup.references() == 4);

  // Failed and successful startups each take one reference; only the last cleanup stops.
  for (int i = 0; i < 3; ++i) {
    CHECK_FALSE(startup.Cleanup(client).has_value());
    CHECK(client.stats().state == relay_link::Association::kLive);
  }
  const auto final_stats = startup.Cleanup(client);
  REQUIRE(final_stats);
  CHECK(final_stats->state == relay_link::Association::kLive);
  CHECK(startup.references() == 0);
  CHECK(client.stats().state == relay_link::Association::kStopped);
  CHECK(client.Send(socket.native_handle(), {0xFFFFFFFFu, 1000}, payload) == 3);
  CHECK(client.stats().dropped_after_cleanup == 1);
  CHECK(startup.Cleanup(client).has_value());
  CHECK(startup.references() == 0);
}
