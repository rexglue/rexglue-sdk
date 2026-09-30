/**
 * @file        tests/core/core_test.cpp
 * @brief       Tests that rexcore stands alone with no module loaded
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/api.h>
#include <rex/cli_arg_list.h>
#include <rex/cvar/registry.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>

namespace {

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() { rex::Shutdown(); }
};

}  // namespace

TEST_CASE_METHOD(Fixture, "Init with an empty module list succeeds and Shutdown returns",
                 "[core]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  REQUIRE(api.value()->config().modules.empty());
  rex::Shutdown();
  REQUIRE(rex::Init(rex::Config{}));
}

TEST_CASE_METHOD(Fixture, "The log, clock and perf scopes are registered and no module scope is",
                 "[core]") {
  auto api = rex::Init(rex::Config{});
  REQUIRE(api);
  auto scopes = api.value()->Get<rex::cvar::ICvarRegistry>()->Scopes();
  std::ranges::sort(scopes);
  REQUIRE(scopes == std::vector<std::string_view>{"clock", "log", "perf"});
}

TEST_CASE_METHOD(Fixture, "ParseCommandLine fills Config and cvar values without a module",
                 "[core]") {
  rex::Config cfg;
  char a0[] = "app";
  char a1[] = "--game_data_root=C:/core";
  char a2[] = "--log.level=debug";
  char* argv[] = {a0, a1, a2};
  auto rest = rex::ParseCommandLine(cfg, rex::CLIArgList::Parse(3, argv));
  REQUIRE(rest.empty());
  REQUIRE(cfg.paths.game_data_root == "C:/core");
  auto api = rex::Init(cfg);
  REQUIRE(api);
  REQUIRE(api.value()->Get<rex::cvar::ICvarRegistry>()->Get("log.level").value() == "debug");
}

TEST_CASE_METHOD(Fixture, "Logging writes through a capture sink with no module loaded", "[core]") {
  REQUIRE(rex::Init(rex::Config{}));
  auto sink = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(sink);
  REXLOG_INFO("core test entry {}", 42);
  std::vector<rex::LogEntry> entries;
  sink->CopyEntries(entries);
  rex::RemoveSink(sink);
  REQUIRE(std::ranges::any_of(entries, [](const rex::LogEntry& entry) {
    return entry.text.find("core test entry 42") != std::string::npos;
  }));
}
