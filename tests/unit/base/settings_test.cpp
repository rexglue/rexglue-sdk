/**
 * @file        tests/unit/base/settings_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/platform/lock_order.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>

#include "base/app_harness.h"

namespace {

using rex::SettingLayer;
using rex::test::HarnessApp;
using rex::test::RunInInit;

class AudioOwner final : public rex::SettingOwner {
 public:
  AudioOwner() : SettingOwner("audio") {}
  REX_SETTING(rex::f64, gain, 1.0, "Gain", rex::Range{0.0, 4.0});
  REX_SETTING(rex::i32, count, 3, "Count");
  REX_SETTING(std::string, mode, "fast", "Mode", rex::Allowed("fast", "slow"));
  REX_SETTING(bool, flag, false, "Flag");
};

class AliasNamed final : public rex::SettingOwner {
 public:
  AliasNamed() : SettingOwner("bind") {}
  REX_SETTING(bool, debug, false, "Debug");
};

class ModeOwner final : public rex::SettingOwner {
 public:
  ModeOwner() : SettingOwner("mode") {}
  REX_SETTING(rex::i32, startup, 1, "Startup", rex::kInitOnly);
  REX_SETTING(rex::i32, launch, 1, "Until launch", rex::kLockedInGame);
  REX_SETTING(rex::i32, restart, 5, "Restart", rex::kRequiresRestart);
  REX_SETTING(rex::i32, live, 1, "Live");
};

class PathOwner final : public rex::SettingOwner {
 public:
  PathOwner() : SettingOwner("tpath") {}
  REX_SETTING(std::filesystem::path, root, "/a", "Root");
  REX_SETTING(std::filesystem::path, log_root, "{tpath.root}/logs", "Logs");

  size_t CountOwned() const { return GetOwnedSettings().size(); }
  bool NamesRegistry() {
    const rex::SettingsRegistry* first = &REX_SETTINGS();
    return first == &REX_SETTINGS();
  }
};

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

}  // namespace

REX_TEST_CASE("settings: value for a later owner applies on register", "[base][settings]") {
  RunInInit([] {
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "audio.gain", "2.0"));
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.gain.Get() == 2.0);
    REQUIRE(audio.gain.GetLayer() == SettingLayer::kFile);
  });
}

REX_TEST_CASE("settings: alias maps an old name", "[base][settings]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REX_SETTINGS().AddAlias("apu.old", "audio.gain");
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "apu.old", "0.5"));
    REQUIRE(audio.gain.Get() == 0.5);
  });
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  int notices = 0;
  for (const auto& line : lines) {
    notices += line.text.find("apu.old") != std::string::npos;
  }
  REQUIRE(notices == 1);
}

REX_TEST_CASE("a setting cannot take a name that is an alias", "[base][settings]") {
  RunInInit([] {
    AliasNamed named;
    auto refused = REX_SETTINGS().Register(named);
    REQUIRE_FALSE(refused);
    const std::string message = refused.error().GetDescription();
    REQUIRE(message.find("bind.debug") != std::string::npos);
    REQUIRE(message.find("ui.bind.debug") != std::string::npos);

    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REX_SETTINGS().AddAlias("apu.renamed", "audio.gain");
    REQUIRE(REX_SETTINGS().Find("audio.gain"));
  });
}

REX_TEST_CASE("settings: startup only refuses after freeze", "[base][settings]") {
  ModeOwner modes;
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REX_TRY_VOID(settings.Register(modes));
    REQUIRE(modes.startup.Set(2));
    return rex::Ok();
  };
  app.init = [&]() -> rex::Result<void> {
    REQUIRE(modes.startup.IsFrozen());
    REQUIRE_FALSE(modes.startup.Set(3));
    REQUIRE(modes.startup.Get() == 2);
    REQUIRE(modes.live.Set(9));
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("settings: until launch writes at the title layer", "[base][settings]") {
  RunInInit([] {
    ModeOwner modes;
    REQUIRE(REX_SETTINGS().Register(modes));
    REQUIRE(modes.launch.Set(6));
    REQUIRE(modes.launch.GetLayer() == SettingLayer::kTitle);
    REQUIRE(modes.live.Set(4));
    REQUIRE(modes.live.Get() == 4);
  });
}

REX_TEST_CASE("settings: restart stages the value", "[base][settings]") {
  RunInInit([] {
    ModeOwner modes;
    REQUIRE(REX_SETTINGS().Register(modes));
    REQUIRE(modes.restart.Set(3));
    REQUIRE(modes.restart.Get() == 5);
    REQUIRE(modes.restart.GetText() == "3");
    REQUIRE(modes.restart.IsRestartPending());
    const auto pending = REX_SETTINGS().GetRestartPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0] == "mode.restart");
  });
}

REX_TEST_CASE("settings: lock wins over runtime", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Lock(1));
    REQUIRE_FALSE(audio.count.SetText(SettingLayer::kRuntime, "2"));
    REQUIRE_FALSE(audio.count.Reset());
    REQUIRE(audio.count.Get() == 1);
    REQUIRE(audio.count.GetLayer() == SettingLayer::kLock);
  });
}

REX_TEST_CASE("settings: higher layers win", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kCommandLine, "audio.count", "30"));
    REQUIRE(REX_SETTINGS().SetText(SettingLayer::kFile, "audio.count", "20"));
    REQUIRE(audio.count.Get() == 30);
    REQUIRE_FALSE(audio.count.SetText(SettingLayer::kTitle, "10"));
    REQUIRE(audio.count.SetText(SettingLayer::kRuntime, "40"));
    REQUIRE(audio.count.Get() == 40);
  });
}

REX_TEST_CASE("settings: path template resolves at read", "[base][settings]") {
  RunInInit([] {
    PathOwner paths;
    REQUIRE(REX_SETTINGS().Register(paths));
    REQUIRE(paths.log_root.GetText() == "{tpath.root}/logs");
    REQUIRE(paths.log_root.Get() == std::filesystem::path("/a/logs"));
    REQUIRE(paths.root.Set("/b"));
    REQUIRE(paths.log_root.Get() == std::filesystem::path("/b/logs"));
  });
}

REX_TEST_CASE("settings: owner name does not hide the registry", "[base][settings]") {
  PathOwner paths;
  REQUIRE(paths.NamesRegistry());
  REQUIRE(paths.CountOwned() == 2);
}

REX_TEST_CASE("settings: range, allowed texts and types are checked", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE_FALSE(audio.gain.Set(4.5));
    REQUIRE_FALSE(audio.gain.Set(-0.1));
    REQUIRE(audio.gain.Set(4.0));
    REQUIRE_FALSE(audio.mode.Set("medium"));
    REQUIRE(audio.mode.Set("slow"));
    REQUIRE_FALSE(REX_SETTINGS().SetText("audio.count", "twelve"));
    REQUIRE_FALSE(REX_SETTINGS().SetText("audio.flag", "maybe"));
    REQUIRE(REX_SETTINGS().SetText("audio.flag", "true"));
    REQUIRE(audio.flag.Get());
    REQUIRE(REX_SETTINGS().SetText("audio.nothing", "1").error().category ==
            rex::ErrorCategory::kNotFound);
  });
}

REX_TEST_CASE("settings: a change watch runs until it is destroyed", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    int calls = 0;
    rex::i32 last = 0;
    {
      auto watch = audio.count.OnChange([&](const rex::i32& value) {
        ++calls;
        last = value;
      });
      REQUIRE(audio.count.Set(2));
      REQUIRE(audio.count.Set(4));
      REQUIRE(calls == 2);
      REQUIRE(last == 4);
    }
    REQUIRE(audio.count.Set(5));
    REQUIRE(calls == 2);
  });
}

REX_TEST_CASE("settings: a write that keeps the value does not call the watch",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    int calls = 0;
    auto watch = audio.count.OnChange([&](const rex::i32&) { ++calls; });
    REQUIRE(audio.count.Set(3));
    REQUIRE(calls == 0);
    REQUIRE(audio.count.Set(4));
    REQUIRE(calls == 1);
    REQUIRE(audio.count.Set(4));
    REQUIRE(calls == 1);
    REQUIRE(audio.count.Reset());
    REQUIRE(calls == 2);
    REQUIRE(audio.count.Reset());
    REQUIRE(calls == 2);
    REQUIRE(audio.count.SetText(SettingLayer::kFile, "3"));
    REQUIRE(calls == 2);
    REQUIRE(audio.count.GetLayer() == SettingLayer::kFile);
    REQUIRE_FALSE(audio.count.SetText(SettingLayer::kTitle, "5"));
    REQUIRE(audio.count.Get() == 3);
    REQUIRE(calls == 2);
  });
}

REX_TEST_CASE("settings: save writes changed settings and load reads them back",
              "[base][settings]") {
  const auto file = TempFile("rex_settings_save_test.toml");
  RunInInit([&] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Set(12));
    REQUIRE(audio.mode.Set("slow"));
    REQUIRE(audio.gain.Set(0.75));
    REQUIRE(REX_SETTINGS().Save(file));
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    REQUIRE(text.str().find("count = 12") != std::string::npos);
    REQUIRE(text.str().find("flag") == std::string::npos);
  });
  AudioOwner audio;
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REX_TRY_VOID(settings.Register(audio));
    REQUIRE(audio.count.Get() == 3);
    REX_TRY_VOID(settings.Load(file));
    REQUIRE(audio.count.Get() == 12);
    REQUIRE(audio.mode.Get() == "slow");
    REQUIRE(audio.gain.Get() == 0.75);
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
  std::filesystem::remove(file);
}

REX_TEST_CASE("settings: a duplicate full name is refused and names both owners",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner first;
    AudioOwner second;
    REQUIRE(REX_SETTINGS().Register(first));
    auto result = REX_SETTINGS().Register(second);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find("audio.gain") != std::string::npos);
  });
}

REX_TEST_CASE("settings: a command runs through the registry", "[base][settings]") {
  class WithCommand final : public rex::SettingOwner {
   public:
    WithCommand() : SettingOwner("cmdowner") {}
    REX_COMMAND(echo, "Echoes", [this](std::string_view args) {
      text = std::string(args);
      return rex::Result<void>();
    });
    std::string text;
  };
  RunInInit([] {
    WithCommand owner;
    REQUIRE(REX_SETTINGS().Register(owner));
    REQUIRE(REX_SETTINGS().Run("cmdowner.echo", "hello"));
    REQUIRE(owner.text == "hello");
    REQUIRE_FALSE(REX_SETTINGS().Run("cmdowner.nothing", ""));
  });
}

namespace {

class PairOwner final : public rex::SettingOwner {
 public:
  PairOwner() : SettingOwner("pair") {}
  REX_SETTING(rex::i32, left, 0, "Left");
  REX_SETTING(rex::i32, right, 0, "Right");
};

std::atomic<int> g_order_violations{0};
void CountOrderViolation(rex::platform::LockRank, rex::platform::LockRank) {
  ++g_order_violations;
}

/** Counts lock order violations while it lives, so a debug report does not assert. */
class OrderViolationCount {
 public:
  OrderViolationCount() : previous_(rex::platform::SetLockOrderHandler(&CountOrderViolation)) {
    g_order_violations = 0;
  }
  ~OrderViolationCount() { rex::platform::SetLockOrderHandler(previous_); }
  OrderViolationCount(const OrderViolationCount&) = delete;
  OrderViolationCount& operator=(const OrderViolationCount&) = delete;

  /** Returns the violations a debug build reports for a count of refusals. */
  static int Expected(int refusals) {
#ifdef REXGLUE_LOCK_RANK_CHECKS
    return refusals;
#else
    (void)refusals;
    return 0;
#endif
  }

 private:
  rex::platform::LockOrderHandler previous_;
};

}  // namespace

REX_TEST_CASE("settings: a bound watch receives the current value before Bind returns",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Set(6));
    std::vector<rex::i32> seen;
    auto watch = audio.count.Bind([&](const rex::i32& value) { seen.push_back(value); });
    REQUIRE(seen == std::vector<rex::i32>{6});
    std::vector<std::string> group;
    auto both = rex::Bind(
        [&](const rex::i32& count, const std::string& mode) {
          group.push_back(std::format("{} {}", count, mode));
        },
        audio.count, audio.mode);
    REQUIRE(group == std::vector<std::string>{"6 fast"});
  });
}

REX_TEST_CASE("settings: OnChange is not called when it subscribes", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Set(6));
    std::vector<rex::i32> seen;
    auto watch = audio.count.OnChange([&](const rex::i32& v) { seen.push_back(v); });
    REQUIRE(seen.empty());
    REQUIRE(audio.count.Set(7));
    REQUIRE(seen == std::vector<rex::i32>{7});
  });
}

REX_TEST_CASE("settings: Bind is called once when it subscribes and once after each change",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<rex::i32> seen;
    auto watch = audio.count.Bind([&](const rex::i32& v) { seen.push_back(v); });
    REQUIRE(seen == std::vector<rex::i32>{3});
    REQUIRE(audio.count.Set(4));
    REQUIRE(audio.count.Set(4));
    REQUIRE(audio.count.Set(5));
    REQUIRE(seen == std::vector<rex::i32>{3, 4, 5});
  });
}

REX_TEST_CASE(
    "settings: the group and list Bind are called once when they subscribe and after a "
    "change",
    "[base][settings]") {
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    REQUIRE(pair.left.Set(1));
    std::vector<std::string> group_seen;
    auto group = rex::Bind(
        [&](const rex::i32& left, const rex::i32& right) {
          group_seen.push_back(std::format("{} {}", left, right));
        },
        pair.left, pair.right);
    std::vector<rex::Setting<rex::i32>*> members{&pair.left, &pair.right};
    std::vector<std::vector<rex::i32>> list_seen;
    auto list = rex::Bind<rex::i32>(
        [&](std::span<const rex::i32> v) { list_seen.emplace_back(v.begin(), v.end()); },
        std::span<rex::Setting<rex::i32>* const>(members));
    REQUIRE(group_seen == std::vector<std::string>{"1 0"});
    REQUIRE(list_seen == std::vector<std::vector<rex::i32>>{{1, 0}});
    REQUIRE(pair.right.Set(2));
    REQUIRE(group_seen == std::vector<std::string>{"1 0", "1 2"});
    REQUIRE(list_seen == std::vector<std::vector<rex::i32>>{{1, 0}, {1, 2}});
  });
}

REX_TEST_CASE(
    "settings: a write racing OnChange reaches a later call in order and the last write is seen",
    "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    REQUIRE(audio.count.Set(0));
    constexpr rex::i32 kLast = 2000;
    std::latch go(1);
    std::atomic<int> refused{0};
    std::jthread writer([&] {
      go.wait();
      for (rex::i32 v = 1; v <= kLast; ++v) {
        refused += !audio.count.Set(v).has_value();
      }
    });
    std::mutex seen_mutex;
    std::vector<rex::i32> seen;
    go.count_down();
    auto watch = audio.count.OnChange([&](const rex::i32& v) {
      std::lock_guard lock(seen_mutex);
      seen.push_back(v);
    });
    writer.join();
    REQUIRE(refused == 0);
    std::lock_guard lock(seen_mutex);
    // The calls run in write order, and when any ran, the last one carries the last value written.
    REQUIRE(std::ranges::is_sorted(seen, std::less_equal<>{}));
    REQUIRE((seen.empty() || seen.back() == kLast));
  });
}

REX_TEST_CASE(
    "settings: OnChange and Bind inside a change function report no lock order and Bind "
    "calls after it",
    "[base][settings]") {
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    std::vector<std::string> order;
    std::optional<rex::ScopeGuard> changed;
    std::optional<rex::ScopeGuard> bound;
    auto outer = pair.left.OnChange([&](const rex::i32& value) {
      order.push_back(std::format("outer {}", value));
      changed.emplace(pair.right.OnChange(
          [&](const rex::i32& v) { order.push_back(std::format("changed {}", v)); }));
      order.push_back("changed returned");
      bound.emplace(
          pair.right.Bind([&](const rex::i32& v) { order.push_back(std::format("bound {}", v)); }));
      order.push_back("outer end");
    });
    const OrderViolationCount violations;
    REQUIRE(pair.left.Set(1));
    REQUIRE(g_order_violations == 0);
    REQUIRE(order ==
            std::vector<std::string>{"outer 1", "changed returned", "outer end", "bound 0"});
  });
}

REX_TEST_CASE("settings: a watch added inside a change call receives its first call after it",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<std::string> order;
    std::optional<rex::ScopeGuard> inner;
    auto outer = audio.count.Bind([&](const rex::i32& value) {
      order.push_back(std::format("outer {}", value));
      if (value == 4 && !inner) {
        inner = audio.mode.Bind(
            [&](const std::string& mode) { order.push_back(std::format("inner {}", mode)); });
        order.push_back("added");
      }
    });
    REQUIRE(audio.count.Set(4));
    REQUIRE(order == std::vector<std::string>{"outer 3", "outer 4", "added", "inner fast"});
  });
}

REX_TEST_CASE("settings: writes from several threads end with every watch on the last value",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    constexpr rex::i32 kThreads = 4;
    constexpr rex::i32 kWrites = 200;
    std::vector<rex::i32> first;
    std::vector<rex::i32> second;
    auto first_watch = audio.count.Bind([&](const rex::i32& value) { first.push_back(value); });
    auto second_watch = audio.count.Bind([&](const rex::i32& value) { second.push_back(value); });
    std::latch start(kThreads);
    std::atomic<int> refused{0};
    {
      std::vector<std::jthread> writers;
      writers.reserve(kThreads);
      for (rex::i32 t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
          start.arrive_and_wait();
          for (rex::i32 i = 1; i <= kWrites; ++i) {
            refused += !audio.count.Set((t + 1) * 1000 + i).has_value();
          }
        });
      }
    }
    REQUIRE(refused == 0);
    REQUIRE(first.back() == audio.count.Get());
    REQUIRE(second.back() == audio.count.Get());
    for (const std::vector<rex::i32>* seen : {&first, &second}) {
      std::array<rex::i32, kThreads> last{};
      for (size_t i = 1; i < seen->size(); ++i) {
        const rex::i32 value = (*seen)[i];
        const rex::i32 t = value / 1000 - 1;
        REQUIRE(t >= 0);
        REQUIRE(t < kThreads);
        REQUIRE(value % 1000 > last[t]);
        last[t] = value % 1000;
      }
    }
  });
}

REX_TEST_CASE("settings: the watches of a setting run in the order they were added",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<int> order;
    std::vector<rex::ScopeGuard> watches;
    for (int id = 1; id <= 4; ++id) {
      watches.push_back(audio.count.Bind([&order, id](const rex::i32&) { order.push_back(id); }));
    }
    order.clear();
    REQUIRE(audio.count.Set(8));
    REQUIRE(order == std::vector<int>{1, 2, 3, 4});
    watches[1].Reset();
    order.clear();
    REQUIRE(audio.count.Set(9));
    REQUIRE(order == std::vector<int>{1, 3, 4});
  });
}

REX_TEST_CASE("settings: a second writer returns only after every watch saw its value",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::mutex record_mutex;
    std::vector<rex::i32> slow_seen;
    std::vector<rex::i32> after_seen;
    std::latch entered(1);
    std::latch release(1);
    auto slow = audio.count.Bind([&](const rex::i32& value) {
      {
        std::lock_guard lock(record_mutex);
        slow_seen.push_back(value);
      }
      if (value == 10) {
        entered.count_down();
        release.wait();
      }
    });
    auto after = audio.count.Bind([&](const rex::i32& value) {
      std::lock_guard lock(record_mutex);
      after_seen.push_back(value);
    });
    bool slow_saw = false;
    bool after_saw = false;
    std::atomic<int> refused{0};
    std::jthread first([&] { refused += !audio.count.Set(10).has_value(); });
    entered.wait();
    std::latch second_started(1);
    std::jthread second([&] {
      second_started.count_down();
      refused += !audio.count.Set(20).has_value();
      std::lock_guard lock(record_mutex);
      slow_saw = std::ranges::find(slow_seen, 20) != slow_seen.end();
      after_saw = !after_seen.empty() && after_seen.back() == 20;
    });
    second_started.wait();
    release.count_down();
    first.join();
    second.join();
    REQUIRE(refused == 0);
    REQUIRE(slow_saw);
    REQUIRE(after_saw);
    REQUIRE(slow_seen.back() == 20);
    REQUIRE(after_seen.back() == 20);
  });
}

namespace {

/** Holds each call of a change function until the test releases it. */
class CallStepper {
 public:
  void Arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
  }

  /** Called at the start of each call. Holds the call while the stepper is armed. */
  void Enter() {
    std::unique_lock lock(mutex_);
    if (!armed_) {
      return;
    }
    const int index = ++started_;
    held_runner_ = std::this_thread::get_id();
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_ >= index || !armed_; });
  }

  /** Waits until a call is held or the condition holds. True when a call is held. */
  template <typename Done>
  bool WaitHeldOr(Done done) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return started_ > released_ || done(); });
    return started_ > released_ && !done();
  }

  /** Returns the thread of the call held last. */
  std::thread::id HeldRunner() {
    std::lock_guard lock(mutex_);
    return held_runner_;
  }

  void Release() {
    std::lock_guard lock(mutex_);
    ++released_;
    changed_.notify_all();
  }

  /** Wakes WaitHeldOr after its condition changes. */
  void Wake() {
    std::lock_guard lock(mutex_);
    changed_.notify_all();
  }

  /** Stops holding calls and lets every held call go. */
  void Open() {
    std::lock_guard lock(mutex_);
    armed_ = false;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_ = false;
  int started_ = 0;
  int released_ = 0;
  std::thread::id held_runner_;
};

/** Waits for a flag without blocking, so the thread that sets it is not preempted by a wake. */
void SpinUntil(const std::atomic<bool>& flag) {
  while (!flag.load()) {
    std::this_thread::yield();
  }
}

/**
 * One try of a second write behind a writer that never stops. The second writer reaches the group
 * watch while a call of the first writer is held. When it takes its ticket before that call ends,
 * the next call is its own, and it returns after that call with no further call released.
 *
 * @return False when the second writer reached the watch only after the held call ended, so the
 * try shows nothing.
 */
bool WriteBehindWriterOnce() {
  AudioOwner audio;
  REQUIRE(REX_SETTINGS().Register(audio));
  CallStepper stepper;
  std::atomic<bool> arrived{false};
  // Added before the group watch, so the second writer calls it just before it reaches the group.
  auto first = audio.count.Bind([&](const rex::i32& value) {
    if (value == 100) {
      arrived = true;
    }
  });
  auto group =
      rex::Bind([&](const bool&, const rex::i32&) { stepper.Enter(); }, audio.flag, audio.count);
  stepper.Arm();
  std::atomic<int> refused{0};
  std::jthread writer([&](const std::stop_token& stop) {
    bool value = false;
    while (!stop.stop_requested()) {
      value = !value;
      refused += !audio.flag.SetText(SettingLayer::kRuntime, value ? "true" : "false");
    }
  });
  REQUIRE(stepper.WaitHeldOr([] { return false; }));
  std::jthread second([&] { refused += !audio.count.Set(100); });
  SpinUntil(arrived);
  stepper.Release();
  REQUIRE(stepper.WaitHeldOr([] { return false; }));
  const bool own_call = stepper.HeldRunner() == second.get_id();
  if (own_call) {
    // The call in progress and its own call: the second write returns with nothing more released.
    stepper.Release();
    second.join();
  }
  stepper.Open();
  if (second.joinable()) {
    second.join();
  }
  writer.request_stop();
  writer.join();
  REQUIRE(refused == 0);
  return own_call;
}

/**
 * One try of a removal behind a writer that never stops. The removal starts while a call is held.
 * When it is stored before that call ends, Remove returns after that call and no call starts again.
 *
 * @return False when the removal was stored only after the held call ended, so the try shows
 * nothing.
 */
bool RemoveBehindWriterOnce() {
  AudioOwner audio;
  REQUIRE(REX_SETTINGS().Register(audio));
  CallStepper stepper;
  std::atomic<int> calls_after_removal{0};
  std::atomic<bool> removed{false};
  std::optional<rex::ScopeGuard> group = rex::Bind(
      [&](const bool&) {
        stepper.Enter();
        calls_after_removal += removed.load();
      },
      audio.flag);
  stepper.Arm();
  std::atomic<int> refused{0};
  std::jthread writer([&](const std::stop_token& stop) {
    bool value = false;
    while (!stop.stop_requested()) {
      value = !value;
      refused += !audio.flag.SetText(SettingLayer::kRuntime, value ? "true" : "false");
    }
  });
  REQUIRE(stepper.WaitHeldOr([] { return false; }));
  std::atomic<bool> removing{false};
  std::jthread remover([&] {
    removing = true;
    group.reset();
    removed = true;
    stepper.Wake();
  });
  SpinUntil(removing);
  stepper.Release();
  // Only the call in progress: once the removal is stored, no call starts, so Remove returns.
  const bool one_call = !stepper.WaitHeldOr([&] { return removed.load(); });
  while (stepper.WaitHeldOr([&] { return removed.load(); })) {
    stepper.Release();
  }
  remover.join();
  stepper.Open();
  writer.request_stop();
  writer.join();
  REQUIRE(calls_after_removal == 0);
  REQUIRE(refused == 0);
  return one_call;
}

/** Tries before a test that needs one try to show its case fails. */
constexpr int kTries = 20;

}  // namespace

REX_TEST_CASE("settings: a write behind a writer that never stops returns after at most two calls",
              "[base][settings]") {
  RunInInit([] {
    bool shown = false;
    for (int i = 0; i < kTries && !shown; ++i) {
      shown = WriteBehindWriterOnce();
    }
    REQUIRE(shown);
  });
}

REX_TEST_CASE("settings: removing a watch behind a writer that never stops waits for one call",
              "[base][settings]") {
  RunInInit([] {
    bool shown = false;
    for (int i = 0; i < kTries && !shown; ++i) {
      shown = RemoveBehindWriterOnce();
    }
    REQUIRE(shown);
  });
}

REX_TEST_CASE("settings: a write inside a change call stores immediately and calls after it",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<std::string> order;
    bool inner_stored = false;
    bool inner_read = false;
    auto writer = audio.count.Bind([&](const rex::i32& value) {
      order.push_back(std::format("writer {}", value));
      if (value == 2) {
        inner_stored = audio.count.Set(7).has_value() && audio.mode.Set("slow").has_value();
        inner_read = audio.count.Get() == 7 && audio.mode.Get() == "slow";
        order.push_back("writer returns");
      }
    });
    auto reader = audio.count.Bind(
        [&](const rex::i32& value) { order.push_back(std::format("reader {}", value)); });
    auto mode = audio.mode.Bind(
        [&](const std::string& value) { order.push_back(std::format("mode {}", value)); });
    order.clear();
    REQUIRE(audio.count.Set(2));
    REQUIRE(inner_stored);
    REQUIRE(inner_read);
    REQUIRE(order == std::vector<std::string>{"writer 2", "writer returns", "reader 7", "writer 7",
                                              "mode slow"});
  });
}

REX_TEST_CASE("settings: change calls that keep rewriting each other stop and log once",
              "[base][settings]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    bool armed = false;
    int calls = 0;
    rex::i32 left_seen = -1;
    auto left = pair.left.Bind([&](const rex::i32& value) {
      left_seen = value;
      if (armed) {
        ++calls;
        REQUIRE(pair.right.Set(value + 1));
      }
    });
    auto right = pair.right.Bind([&](const rex::i32& value) {
      if (armed) {
        ++calls;
        REQUIRE(pair.left.Set(value + 1));
      }
    });
    armed = true;
    REQUIRE(pair.left.Set(1));
    armed = false;
    REQUIRE(calls > 16);
    REQUIRE(calls < 40);
    REQUIRE(pair.left.Get() + pair.right.Get() > 16);
    REQUIRE(pair.left.Set(100));
    REQUIRE(left_seen == 100);
  });
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  int stops = 0;
  for (const auto& line : lines) {
    stops += line.text.find("setting change calls stopped") != std::string::npos;
  }
  REQUIRE(stops == 1);
}

REX_TEST_CASE("settings: a chain of change calls is not cut", "[base][settings]") {
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  RunInInit([] {
    PairOwner pair;
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(pair));
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<std::string> order;
    auto left = pair.left.OnChange([&](const rex::i32& value) {
      order.push_back(std::format("left {}", value));
      REQUIRE(pair.right.Set(value + 1));
    });
    auto right = pair.right.OnChange([&](const rex::i32& value) {
      order.push_back(std::format("right {}", value));
      REQUIRE(audio.count.Set(value + 1));
    });
    auto count = audio.count.OnChange(
        [&](const rex::i32& value) { order.push_back(std::format("count {}", value)); });
    REQUIRE(pair.left.Set(5));
    REQUIRE(order == std::vector<std::string>{"left 5", "right 6", "count 7"});
  });
  sink_guard.Reset();
  std::vector<rex::log::CapturedMessage> lines;
  sink.CopySince(0, lines);
  for (const auto& line : lines) {
    REQUIRE(line.text.find("setting change calls stopped") == std::string::npos);
  }
}

REX_TEST_CASE("settings: a batch calls each watch once when it ends", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<rex::i32> counts;
    std::vector<std::string> groups;
    auto count = audio.count.Bind([&](const rex::i32& value) { counts.push_back(value); });
    auto group = rex::Bind(
        [&](const rex::i32& value, const std::string& mode) {
          groups.push_back(std::format("{} {}", value, mode));
        },
        audio.count, audio.mode);
    counts.clear();
    groups.clear();
    {
      const rex::SettingsBatch batch;
      REQUIRE(audio.count.Set(4));
      REQUIRE(audio.count.Set(5));
      REQUIRE(audio.mode.Set("slow"));
      REQUIRE(audio.count.Get() == 5);
      REQUIRE(audio.mode.Get() == "slow");
      REQUIRE(counts.empty());
      REQUIRE(groups.empty());
    }
    REQUIRE(counts == std::vector<rex::i32>{5});
    REQUIRE(groups == std::vector<std::string>{"5 slow"});
  });
}

REX_TEST_CASE("settings: a batch inside a batch joins it", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::vector<rex::i32> counts;
    auto count = audio.count.Bind([&](const rex::i32& value) { counts.push_back(value); });
    counts.clear();
    {
      const rex::SettingsBatch outer;
      REQUIRE(audio.count.Set(4));
      {
        const rex::SettingsBatch inner;
        REQUIRE(audio.count.Set(6));
      }
      REQUIRE(counts.empty());
      REQUIRE(audio.count.Set(7));
    }
    REQUIRE(counts == std::vector<rex::i32>{7});
  });
}

REX_TEST_CASE("settings: a watch removed inside a change call is not called again",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    int self_calls = 0;
    int later_calls = 0;
    bool armed = false;
    std::optional<rex::ScopeGuard> self;
    std::optional<rex::ScopeGuard> later;
    self = audio.count.Bind([&](const rex::i32&) {
      ++self_calls;
      if (armed) {
        self.reset();
        later.reset();
      }
    });
    later = audio.count.Bind([&](const rex::i32&) { ++later_calls; });
    armed = true;
    REQUIRE(audio.count.Set(2));
    REQUIRE(audio.count.Set(4));
    REQUIRE(self_calls == 2);
    REQUIRE(later_calls == 1);
  });
}

REX_TEST_CASE("settings: removing a watch waits for its call on another thread",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    std::latch entered(1);
    std::latch removing(1);
    std::atomic<bool> removed{false};
    std::atomic<bool> removed_while_running{true};
    std::atomic<int> calls{0};
    std::optional<rex::ScopeGuard> watch = audio.count.Bind([&](const rex::i32& value) {
      if (value != 2) {
        return;
      }
      ++calls;
      entered.count_down();
      removing.wait();
      removed_while_running = removed.load();
    });
    std::atomic<bool> stored{false};
    std::jthread writer([&] { stored = audio.count.Set(2).has_value(); });
    entered.wait();
    std::jthread remover([&] {
      removing.count_down();
      watch.reset();
      removed = true;
    });
    writer.join();
    remover.join();
    REQUIRE(stored.load());
    REQUIRE_FALSE(removed_while_running.load());
    REQUIRE(audio.count.Set(4));
    REQUIRE(audio.count.Set(2));
    REQUIRE(calls == 1);
  });
}

REX_TEST_CASE("settings: a group watch runs one call at a time and ends on the last values",
              "[base][settings]") {
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    constexpr rex::i32 kWrites = 400;
    std::atomic<bool> inside{false};
    std::atomic<int> overlapped{0};
    rex::i32 left_seen = -1;
    rex::i32 right_seen = -1;
    auto group = rex::Bind(
        [&](const rex::i32& left, const rex::i32& right) {
          overlapped += inside.exchange(true) ? 1 : 0;
          left_seen = left;
          right_seen = right;
          inside = false;
        },
        pair.left, pair.right);
    std::latch start(2);
    std::atomic<int> refused{0};
    {
      std::jthread lefts([&] {
        start.arrive_and_wait();
        for (rex::i32 i = 1; i <= kWrites; ++i) {
          refused += !pair.left.Set(i).has_value();
        }
      });
      std::jthread rights([&] {
        start.arrive_and_wait();
        for (rex::i32 i = 1; i <= kWrites; ++i) {
          refused += !pair.right.Set(i).has_value();
        }
      });
    }
    REQUIRE(refused == 0);
    REQUIRE(overlapped == 0);
    REQUIRE(left_seen == kWrites);
    REQUIRE(right_seen == kWrites);
  });
}

REX_TEST_CASE("settings: a list watch receives the values in list order", "[base][settings]") {
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    std::vector<rex::Setting<rex::i32>*> members{&pair.right, &pair.left};
    std::vector<std::vector<rex::i32>> seen;
    auto group = rex::Bind<rex::i32>(
        [&](std::span<const rex::i32> values) { seen.emplace_back(values.begin(), values.end()); },
        std::span<rex::Setting<rex::i32>* const>(members));
    REQUIRE(pair.left.Set(3));
    REQUIRE(seen == std::vector<std::vector<rex::i32>>{{0, 0}, {0, 3}});
  });
}

REX_TEST_CASE("settings: a destroyed setting stops its watch and the guard removes it after",
              "[base][settings]") {
  RunInInit([] {
    auto pair = std::make_unique<PairOwner>();
    int calls = 0;
    auto watch = pair->left.Bind([&](const rex::i32&) { ++calls; });
    pair.reset();
    watch.Reset();
    REQUIRE(calls == 1);
  });
}

namespace {

class WritingSink final : public rex::log::ILogSink {
 public:
  explicit WritingSink(AudioOwner& audio) : audio_(audio) {}

  void Write(const rex::log::Message& message) override {
    if (message.text == "write from a sink") {
      refused = !audio_.count.Set(9).has_value();
    }
  }

  void Flush() override {}

  std::atomic<bool> refused{false};

 private:
  AudioOwner& audio_;
};

}  // namespace

REX_TEST_CASE("settings: a write from inside a log sink is refused", "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    WritingSink sink(audio);
    auto sink_guard = REX_LOGGER().AddSink(sink);
    {
      const OrderViolationCount violations;
      REXLOG_WARN("write from a sink");
      REQUIRE(g_order_violations == OrderViolationCount::Expected(1));
    }
    sink_guard.Reset();
    REQUIRE(sink.refused.load());
    REQUIRE(audio.count.Get() == 3);
  });
}

REX_TEST_CASE("settings: a write that calls a watch under a ranked lock is a lock order violation",
              "[base][settings]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    int calls = 0;
    auto watch = audio.count.Bind([&](const rex::i32&) { ++calls; });
    const OrderViolationCount violations;
    {
      rex::platform::RankedMutex held(rex::platform::lock_rank::kObjectState);
      std::lock_guard lock(held);
      REQUIRE(audio.count.Set(5));
    }
    REQUIRE(calls == 2);
    REQUIRE(g_order_violations == OrderViolationCount::Expected(1));
  });
}

REX_TEST_CASE("settings: a write either stores before the freeze returns or is refused",
              "[base][settings]") {
  ModeOwner modes;
  std::atomic<rex::i32> last_stored{1};
  std::atomic<bool> stop{false};
  std::optional<std::jthread> writer;
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry& settings) -> rex::Result<void> {
    REX_TRY_VOID(settings.Register(modes));
    std::latch writing(1);
    writer.emplace([&] {
      rex::i32 next = 2;
      bool signalled = false;
      while (!stop.load()) {
        if (modes.startup.Set(next)) {
          last_stored = next;
          ++next;
        }
        if (!signalled) {
          writing.count_down();
          signalled = true;
        }
      }
    });
    writing.wait();
    return rex::Ok();
  };
  rex::i32 at_init = 0;
  app.init = [&]() -> rex::Result<void> {
    at_init = modes.startup.Get();
    return rex::Ok();
  };
  REQUIRE(app.Start());
  stop = true;
  writer.reset();
  REQUIRE(modes.startup.IsFrozen());
  REQUIRE(at_init == last_stored.load());
  REQUIRE(modes.startup.Get() == last_stored.load());
  app.Finish();
}

REX_TEST_CASE("settings: thread model costs", "[base][settings][benchmark]") {
  RunInInit([] {
    AudioOwner audio;
    REQUIRE(REX_SETTINGS().Register(audio));
    rex::u64 baseline = 0;
    BENCHMARK("harness baseline, a no-op") {
      return ++baseline;
    };
    BENCHMARK("SetText with the same value") {
      return audio.count.SetText(SettingLayer::kRuntime, "3").has_value();
    };
    int calls = 0;
    auto watch = audio.count.Bind([&](const rex::i32&) { ++calls; });
    bool ten = false;
    BENCHMARK("SetText with a new value and one watch") {
      ten = !ten;
      return audio.count.SetText(SettingLayer::kRuntime, ten ? "10" : "11").has_value();
    };
    BENCHMARK("Get of a scalar") {
      return audio.count.Get();
    };
    BENCHMARK("Get of a string") {
      return audio.mode.Get();
    };
    // The slow watch spins to a deadline, since a sleep can take a whole timer tick on Windows. It
    // counts its calls by the thread that makes them.
    const std::thread::id bench_thread = std::this_thread::get_id();
    std::atomic<int> bench_calls{0};
    std::atomic<int> writer_calls{0};
    std::atomic<int> active{0};
    auto slow = audio.flag.Bind([&](const bool&) {
      ++active;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
      while (std::chrono::steady_clock::now() < deadline) {}
      // Counted before it stops being active, so a reader that reads active and then the count
      // sees the call in one of them.
      ++(std::this_thread::get_id() == bench_thread ? bench_calls : writer_calls);
      --active;
    });
    std::atomic<int> refused{0};
    std::latch looping(1);
    std::jthread writer([&audio, &refused, &looping](const std::stop_token& stop) {
      bool value = false;
      looping.count_down();
      while (!stop.stop_requested()) {
        value = !value;
        refused += !audio.flag.SetText(SettingLayer::kRuntime, value ? "true" : "false");
      }
    });
    looping.wait();
    // Starts while a slow call of the writer is in progress, so at least one is in progress during
    // the case: it ends during the case or is still active after it.
    while (active.load() == 0) {
      std::this_thread::yield();
    }
    const int writer_before_other = writer_calls.load();
    BENCHMARK("SetText of another setting behind a 1 ms watch on another thread") {
      ten = !ten;
      return audio.count.SetText(SettingLayer::kRuntime, ten ? "10" : "11").has_value();
    };
    const int active_after_other = active.load();
    const int slow_during_other = writer_calls.load() - writer_before_other + active_after_other;
    REQUIRE(slow_during_other > 0);
    const int bench_before_same = bench_calls.load();
    const int writer_before_same = writer_calls.load();
    bool flag = false;
    BENCHMARK("SetText of the same setting behind a 1 ms watch on another thread") {
      flag = !flag;
      return audio.flag.SetText(SettingLayer::kRuntime, flag ? "true" : "false").has_value();
    };
    const int bench_same = bench_calls.load() - bench_before_same;
    const int writer_same = writer_calls.load() - writer_before_same;
    writer.request_stop();
    writer.join();
    std::printf(
        "slow watch calls: %d of the writer in progress during the other setting case; in the same "
        "setting case %d by the benchmark thread and %d by the writer\n",
        slow_during_other, bench_same, writer_same);
    REQUIRE(calls > 0);
    REQUIRE(refused == 0);
  });
}

REX_TEST_CASE("settings: a change call that removes a watch running on another thread waits for it",
              "[base][settings]") {
  RunInInit([] {
    PairOwner pair;
    REQUIRE(REX_SETTINGS().Register(pair));
    bool armed = false;
    std::latch entered(1);
    std::latch removing(1);
    std::atomic<bool> right_done{false};
    std::atomic<bool> done_before_return{false};
    std::optional<rex::ScopeGuard> right = pair.right.Bind([&](const rex::i32&) {
      if (!armed) {
        return;
      }
      entered.count_down();
      removing.wait();
      right_done = true;
    });
    auto left = pair.left.Bind([&](const rex::i32&) {
      if (!armed) {
        return;
      }
      removing.count_down();
      right.reset();
      done_before_return = right_done.load();
    });
    armed = true;
    std::atomic<int> refused{0};
    {
      std::jthread second([&] { refused += !pair.right.Set(1).has_value(); });
      entered.wait();
      std::jthread first([&] { refused += !pair.left.Set(1).has_value(); });
    }
    REQUIRE(refused == 0);
    REQUIRE(done_before_return.load());
  });
}
