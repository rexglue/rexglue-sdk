/**
 * @file        tests/unit/core/logger_test.cpp
 * @brief       Logger level resolution, registration, formatting and sink routing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"
#include "file_text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/logging/format.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/logging/sink.h>
#include <rex/platform/thread.h>

REXLOG_CATEGORY(test_alpha, "Logger test category alpha")
REXLOG_CATEGORY(test_beta, "Logger test category beta")
REXLOG_CATEGORY(test_gamma, "Logger test category gamma")
REXLOG_CATEGORY(test_orphan, "Logger test category nobody registers")
REXLOG_CATEGORY(level, "Logger test category that collides with log.level")
REXLOG_TABLE(kTestLog, test_alpha, test_beta)
REXLOG_TABLE(kCollidingLog, level)

namespace {

using rex::test::ReadText;

using rex::log::Level;

class RecordingSink final : public rex::log::ISink {
 public:
  void Write(const rex::log::Message& message) override {
    messages.push_back({message.level, std::string(message.category), std::string(message.text),
                        message.time, message.thread_id, message.sequence});
  }
  void Flush() override {}

  bool Contains(std::string_view text) const {
    return std::ranges::any_of(messages, [&](const rex::log::CapturedMessage& message) {
      return message.text.find(text) != std::string::npos;
    });
  }

  std::vector<rex::log::CapturedMessage> messages;
};

struct TempFolder : rex::test::TempFolder {
  TempFolder() : rex::test::TempFolder("rex_logger_test_") {}
};

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() {
    if (api) {
      CHECK(GetCvars().ResetToDefault("log.level"));
      CHECK(GetCvars().ResetToDefault("log.file"));
    }
    rex::Shutdown();
  }

  rex::ILogger& Start(const rex::Config& config = {}) {
    auto started = rex::Init(config);
    REQUIRE(started);
    api = *started;
    auto* logger = api->Get<rex::ILogger>();
    REQUIRE(logger);
    return *logger;
  }

  rex::cvar::ICvarRegistry& GetCvars() const { return *api->Get<rex::cvar::ICvarRegistry>(); }

  rex::API* api = nullptr;
};

}  // namespace

REX_TEST_CASE("Level names round trip and reject other forms", "[core][logger]") {
  for (Level level : {Level::kTrace, Level::kDev, Level::kDebug, Level::kInfo, Level::kWarn,
                      Level::kError, Level::kCritical, Level::kOff}) {
    auto parsed = rex::log::ParseLevel(rex::log::LevelName(level));
    REQUIRE(parsed);
    REQUIRE(*parsed == level);
  }
  REQUIRE(rex::log::ParseLevel("WARN").value() == Level::kWarn);
  REQUIRE_FALSE(rex::log::ParseLevel("warning"));
  REQUIRE_FALSE(rex::log::ParseLevel("err"));
}

REX_TEST_CASE("Level colors and letters are the 1 palette every text sink uses", "[core][logger]") {
  CHECK(rex::log::LevelColor(Level::kInfo).empty());
  CHECK(rex::log::LevelColor(Level::kOff).empty());
  CHECK(rex::log::LevelColor(Level::kTrace) == "[90m");
  CHECK(rex::log::LevelColor(Level::kWarn) == "[33m");
  CHECK(rex::log::LevelColor(Level::kCritical) == "[97;41m");
  CHECK(rex::log::kColorReset == "[0m");
  CHECK(rex::log::LevelLetter(Level::kTrace) == 'T');
  CHECK(rex::log::LevelLetter(Level::kCritical) == 'C');
  CHECK(rex::log::LevelLetter(Level::kOff) == '-');
}

REX_TEST_CASE_METHOD(Fixture, "A category follows the default until it has its own level",
                     "[core][logger]") {
  rex::ILogger& logger = Start();
  REQUIRE(logger.Register(rex::log::kTestLog));
  REQUIRE(logger.GetLevel("test_alpha").value() == Level::kInfo);
  REQUIRE(logger.SetLevel("test_alpha", Level::kDebug));
  REQUIRE(logger.GetLevel("test_alpha").value() == Level::kDebug);
  REQUIRE(GetCvars().Set("log.level", "error"));
  REQUIRE(logger.GetLevel("test_alpha").value() == Level::kDebug);
  REQUIRE(logger.GetLevel("test_beta").value() == Level::kError);
  REQUIRE(logger.SetLevel("test_alpha", std::nullopt));
  REQUIRE(logger.GetLevel("test_alpha").value() == Level::kError);
  REQUIRE_FALSE(logger.GetLevel("no_such_category"));
  REQUIRE_FALSE(logger.SetLevel("", std::nullopt));
  logger.Unregister(rex::log::kTestLog);
}

REX_TEST_CASE_METHOD(Fixture, "A disabled level does not evaluate its arguments",
                     "[core][logger]") {
  rex::ILogger& logger = Start();
  REQUIRE(logger.Register(rex::log::kTestLog));
  REQUIRE(logger.SetLevel("test_alpha", Level::kWarn));
  int evaluated = 0;
  auto count = [&] { return ++evaluated; };
  REXLOG_DEBUG(test_alpha, "{}", count());
  REQUIRE(evaluated == 0);
  REXLOG_ERROR(test_alpha, "{}", count());
  REQUIRE(evaluated == 1);
  logger.Unregister(rex::log::kTestLog);
}

REX_TEST_CASE_METHOD(Fixture, "Register adds a cvar per category and rejects conflicts",
                     "[core][logger]") {
  rex::ILogger& logger = Start();
  REQUIRE(logger.Register(rex::log::kTestLog));
  REQUIRE(GetCvars().Find("log.test_alpha"));
  REQUIRE_FALSE(logger.Register(rex::log::kTestLog));
  REQUIRE_FALSE(logger.Register(rex::log::kCollidingLog));
  REQUIRE(GetCvars().Set("log.test_beta", "trace"));
  REQUIRE(logger.GetLevel("test_beta").value() == Level::kTrace);
  logger.Unregister(rex::log::kTestLog);
  REQUIRE_FALSE(GetCvars().Find("log.test_alpha"));
}

REX_TEST_CASE_METHOD(Fixture, "A config file value reaches a category registered at start",
                     "[core][logger]") {
  rex::Config config;
  config.log.categories = {&rex::log::test_gamma};
  config.cvars.values.push_back({"log.test_gamma", "debug", rex::cvar::CvarSource::kConfig});
  rex::ILogger& logger = Start(config);
  REQUIRE(logger.GetLevel("test_gamma").value() == Level::kDebug);
}

REX_TEST_CASE_METHOD(Fixture, "A category nobody registered is listed and reported once",
                     "[core][logger]") {
  rex::ILogger& logger = Start();
  RecordingSink sink;
  logger.AddSink(sink);
  REXLOG_ERROR(test_orphan, "orphan line");
  logger.RemoveSink(sink);
  REQUIRE(sink.Contains("orphan line"));
  const auto categories = logger.GetCategories();
  auto orphan =
      std::ranges::find(categories, std::string("test_orphan"), &rex::log::CategoryState::name);
  REQUIRE(orphan != categories.end());
  REQUIRE_FALSE(orphan->registered);
}

REX_TEST_CASE_METHOD(Fixture, "Sinks receive messages in order until removed", "[core][logger]") {
  rex::ILogger& logger = Start();
  RecordingSink sink;
  logger.AddSink(sink);
  logger.AddSink(sink);
  REXLOG_WARN(core, "first");
  REXLOG_WARN(core, "second");
  logger.RemoveSink(sink);
  REXLOG_WARN(core, "third");
  REQUIRE(sink.messages.size() == 2);
  REQUIRE(sink.messages[0].text == "first");
  REQUIRE(sink.messages[1].text == "second");
  REQUIRE(sink.messages[0].sequence < sink.messages[1].sequence);
}

REX_TEST_CASE("FormatLine writes the columns it is asked for and never the level",
              "[core][logger]") {
  rex::log::Message message;
  message.level = Level::kWarn;
  message.category = "gpu";
  message.text = "hello";
  message.time = std::chrono::system_clock::now();
  message.thread_id = 42;
  rex::log::LineFormat plain{rex::log::TimestampStyle::kNone, false, false, false};
  REQUIRE(rex::log::FormatLine(message, plain) == "hello");
  rex::log::LineFormat full{rex::log::TimestampStyle::kClock, true, true, false};
  const std::string line = rex::log::FormatLine(message, full);
  REQUIRE(line.find("gpu") != std::string::npos);
  REQUIRE(line.find("t00042") != std::string::npos);
  REQUIRE(line.ends_with("hello"));
  REQUIRE(line.find("warn") == std::string::npos);
  rex::log::LineFormat dated{rex::log::TimestampStyle::kDateTime, false, false, false};
  REQUIRE(rex::log::FormatLine(message, dated).find('-') == 4);
  rex::log::LineFormat elapsed{rex::log::TimestampStyle::kElapsed, false, false, false};
  REQUIRE(rex::log::FormatLine(message, elapsed).starts_with("+"));
  rex::log::LineFormat colored{rex::log::TimestampStyle::kNone, false, false, true};
  REQUIRE(rex::log::FormatLine(message, colored).starts_with("\x1b["));
}

REX_TEST_CASE("A file sink writes a level letter column", "[core][logger]") {
  TempFolder temp;
  const auto path = temp.path / "direct.log";
  {
    auto sink =
        rex::log::FileSink::Open(path, {rex::log::TimestampStyle::kNone, true, false, false});
    REQUIRE(sink);
    rex::log::Message message;
    message.level = Level::kError;
    message.category = "core";
    message.text = "file line";
    (*sink)->Write(message);
    (*sink)->Flush();
  }
  REQUIRE(ReadText(path).starts_with("E core"));
}

REX_TEST_CASE_METHOD(Fixture, "Sequential log files count up and old runs are pruned",
                     "[core][logger]") {
  TempFolder temp;
  rex::Config config;
  config.log.file.enabled = true;
  config.log.file.folder = temp.path;
  config.log.file.name = "run";
  rex::ILogger& first = Start(config);
  REQUIRE(first.GetFilePath().filename() == "run_001.log");
  REXLOG_ERROR(core, "{}", std::string(4096, 'x'));
  rex::Shutdown();
  rex::ILogger& second = Start(config);
  REQUIRE(second.GetFilePath().filename() == "run_002.log");
  rex::Shutdown();
  config.log.file.folder_budget = 1;
  Start(config);
  REQUIRE_FALSE(std::filesystem::exists(temp.path / "run_001.log"));
}

REX_TEST_CASE("A capture sink returns only newer messages and keeps its capacity",
              "[core][logger]") {
  rex::log::CaptureSink sink;
  rex::log::Message message;
  for (rex::u64 i = 1; i <= rex::log::CaptureSink::kCapacity + 10; ++i) {
    message.sequence = i;
    sink.Write(message);
  }
  std::vector<rex::log::CapturedMessage> out;
  REQUIRE(sink.CopySince(0, out) == rex::log::CaptureSink::kCapacity + 10);
  REQUIRE(out.size() == rex::log::CaptureSink::kCapacity);
  REQUIRE(out.front().sequence == 11);
  sink.CopySince(rex::log::CaptureSink::kCapacity + 5, out);
  REQUIRE(out.size() == 5);
}

REX_TEST_CASE_METHOD(Fixture, "Lines written while stopped reach the next log file",
                     "[core][logger]") {
  TempFolder temp;
  REXLOG_ERROR(core, "written while stopped");
  rex::Config config;
  config.log.file.enabled = true;
  config.log.file.path = temp.path / "replay.log";
  rex::ILogger& logger = Start(config);
  logger.Flush();
  REQUIRE(ReadText(temp.path / "replay.log").find("written while stopped") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "A stub warns once and writes Dev after", "[core][logger]") {
  rex::ILogger& logger = Start();
  REQUIRE(logger.SetLevel("", Level::kTrace));
  RecordingSink sink;
  logger.AddSink(sink);
  std::atomic_flag seen;
  rex::log::detail::WriteStub(seen, "TestStub", {});
  rex::log::detail::WriteStub(seen, "TestStub", "again");
  logger.RemoveSink(sink);
  std::vector<rex::log::CapturedMessage> stubs;
  std::ranges::copy_if(sink.messages, std::back_inserter(stubs), [](const auto& message) {
    return message.text.starts_with("TestStub stub");
  });
  REQUIRE(stubs.size() == 2);
  REQUIRE(stubs[0].level == Level::kWarn);
  REQUIRE(stubs[1].level == Level::kDev);
  REQUIRE(stubs[1].text == "TestStub stub, again");
}

REX_TEST_CASE_METHOD(Fixture, "A log.flush_interval of 0 starts no flush thread",
                     "[core][logger]") {
  TempFolder temp;
  rex::Config config;
  config.log.file.enabled = true;
  config.log.file.path = temp.path / "flush.log";
  config.log.flush_interval = std::chrono::seconds(0);
  Start(config);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const rex::u64 without_flush = rex::platform::detail::GetThreadCount();
  rex::Shutdown();
  api = nullptr;

  config.log.flush_interval = std::chrono::seconds(1);
  config.cvars.values.push_back({"log.flush_interval", "0", rex::cvar::CvarSource::kCommandLine});
  Start(config);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(rex::platform::detail::GetThreadCount() == without_flush);
  CHECK(GetCvars().ResetToDefault("log.flush_interval"));
}

REX_TEST_CASE_METHOD(Fixture, "The log config sets levels and log.file overrides the path",
                     "[core][logger]") {
  TempFolder temp;
  rex::Config config;
  config.log.categories = {&rex::log::test_gamma};
  config.log.levels.emplace("test_gamma", Level::kDebug);
  const auto path = temp.path / "override.log";
  config.cvars.values.push_back({"log.file", path.string(), rex::cvar::CvarSource::kCommandLine});
  rex::ILogger& logger = Start(config);
  REQUIRE(logger.GetLevel("test_gamma").value() == Level::kDebug);
  REQUIRE(logger.GetFilePath() == path);
}

namespace {
struct ThrowingArg {};
}  // namespace

template <>
struct std::formatter<ThrowingArg> {
  constexpr auto parse(std::format_parse_context& context) { return context.begin(); }
  auto format(const ThrowingArg&, std::format_context& context) const {
    throw std::runtime_error("formatter failed");
    return context.out();
  }
};

REX_TEST_CASE_METHOD(Fixture, "A message whose formatting throws logs a fallback line",
                     "[core][logger]") {
  rex::ILogger& logger = Start();
  REQUIRE(logger.Register(rex::log::kTestLog));
  RecordingSink sink;
  logger.AddSink(sink);
  auto write = [] { REXLOG_WARN(test_alpha, "{}", ThrowingArg{}); };
  CHECK_NOTHROW(write());
  logger.RemoveSink(sink);
  logger.Unregister(rex::log::kTestLog);
  CHECK(sink.Contains("log message dropped, formatter failed"));
}
