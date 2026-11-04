/**
 * @file        tests/unit/apu/audio_system_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/app/app.h>
#include <rex/apu/audio_driver.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>

#include "apu/mix.h"
#include "pull_device.h"
#include "temp_folder.h"
#include "ui/ui_harness.h"

namespace {

using Frame = std::array<rex::f32, rex::apu::kAudioFrameSamples>;
using Block = std::vector<rex::f32>;

constexpr rex::u32 kPeriodFrames = rex::apu::kAudioChannelSamples;

struct FakeDevice {
  std::atomic<bool> lost{false};
  std::atomic<bool> stereo_only{false};
  std::atomic<rex::u32> opens{0};
  std::atomic<rex::u32> closes{0};
  rex::test::PullDevice pull;
  std::mutex mutex;
  std::vector<Block> blocks;

  void Reset() {
    lost = false;
    stereo_only = false;
    opens = 0;
    closes = 0;
    pull.Close();
    pull.SetPaused(false);
    pull.SetRequest(kPeriodFrames);
    std::lock_guard lock(mutex);
    blocks.clear();
  }

  size_t GetBlockCount() {
    std::lock_guard lock(mutex);
    return blocks.size();
  }

  std::vector<Block> Take() {
    std::lock_guard lock(mutex);
    return std::exchange(blocks, {});
  }
};

FakeDevice g_device;

class FakeDriver final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "fake"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  rex::Result<rex::u32> OpenDevice(rex::u32 wanted, rex::apu::AudioFill fill) override {
    ++g_device.opens;
    if (g_device.lost) {
      return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform, "no device");
    }
    const rex::u32 channels = g_device.stereo_only ? 2u : wanted;
    g_device.pull.Open(std::move(fill), channels, [](std::span<const rex::f32> samples) {
      std::lock_guard lock(g_device.mutex);
      g_device.blocks.emplace_back(samples.begin(), samples.end());
    });
    return channels;
  }
  void CloseDevice() override {
    ++g_device.closes;
    g_device.pull.Close();
  }
  bool IsDeviceLost() const override { return g_device.lost; }
};

std::unique_ptr<rex::IDriver> CreateFake() {
  return std::make_unique<FakeDriver>();
}

class FakeProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "fake"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 1> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kAudio, "fake", {}, &CreateFake}};
};

class AudioApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit AudioApp(std::vector<std::pair<std::string, std::string>> settings)
      : ObjectApp("audio_test"), settings_(std::move(settings)) {}

  std::function<void(rex::apu::IAudioSystem&)> body;
  std::exception_ptr failure;
  bool windowed = false;

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

  bool Pump() { return RunFrame(); }

  int Finish() {
    const int code = Shutdown();
    if (failure) {
      std::rethrow_exception(failure);
    }
    return code;
  }

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
    providers.push_back(&ui_provider_);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry& registry) override {
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle,
                                               windowed ? "fake" : "none");
    for (const auto& [name, value] : settings_) {
      REX_TRY_VOID(registry.SetText(rex::SettingLayer::kTitle, name, value));
    }
    return rex::Ok();
  }

  rex::Result<void> OnInit() override {
    try {
      if (body) {
        body(GetAudio());
      }
    } catch (...) {
      failure = std::current_exception();
      return rex::Err(rex::ErrorCategory::kRuntime, "the test body threw");
    }
    return rex::Ok();
  }

 private:
  std::vector<std::pair<std::string, std::string>> settings_;
  rex::test::TempFolder folder_{"rex_audio_"};
  FakeProvider provider_;
  rex::test_ui::FakeUIProvider ui_provider_;
};

void Run(std::vector<std::pair<std::string, std::string>> settings,
         std::function<void(rex::apu::IAudioSystem&)> body) {
  g_device.Reset();
  AudioApp app(std::move(settings));
  app.body = std::move(body);
  const bool started = static_cast<bool>(app.Start());
  app.Finish();
  REQUIRE(started);
}

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return condition();
}

Frame Constant(rex::f32 value) {
  Frame frame;
  frame.fill(value);
  return frame;
}

class Feeder {
 public:
  Feeder(rex::apu::IAudioSystem& audio, rex::apu::AudioStreamID stream, const Frame& frame)
      : thread_([this, &audio, stream, frame] {
          while (!stop_) {
            if (audio.GetStats().queued_frames < 3) {
              (void)audio.Submit(stream, frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }) {}

  ~Feeder() {
    stop_ = true;
    thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

rex::f32 At(const Block& block, rex::u32 channels, rex::u32 channel, rex::u32 n) {
  return block[n * channels + channel];
}

rex::f32 Peak(const Block& block, rex::u32 channels, rex::u32 channel) {
  rex::f32 peak = 0;
  for (rex::u32 n = 0; n < rex::apu::kAudioChannelSamples; ++n) {
    peak = std::max(peak, std::fabs(At(block, channels, channel, n)));
  }
  return peak;
}

bool Near(rex::f32 value, rex::f32 expected, rex::f32 margin) {
  return std::fabs(value - expected) <= margin;
}

}  // namespace

REX_TEST_CASE("a stream is credited with no driver", "[apu]") {
  Run({{"apu.driver", "none"}}, [](rex::apu::IAudioSystem& audio) {
    CHECK(audio.GetOutputChannels() == 2);
    std::atomic<int> played{0};
    auto stream = audio.OpenStream([&played] { ++played; });
    REQUIRE(stream);
    Feeder feeder(audio, *stream, Constant(0.25f));
    CHECK(WaitFor([&] { return played >= 12; }));
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("mute silences what reaches the device", "[apu]") {
  Run({{"apu.driver", "fake"}, {"apu.mute", "true"}}, [](rex::apu::IAudioSystem& audio) {
    auto stream = audio.OpenStream([] {});
    REQUIRE(stream);
    Feeder feeder(audio, *stream, Constant(0.25f));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 12; }));
    const auto blocks = g_device.Take();
    for (size_t i = 2; i < blocks.size(); ++i) {
      for (rex::f32 sample : blocks[i]) {
        CHECK(sample == 0.0f);
      }
    }
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("gain and the surround levels scale what reaches the device", "[apu]") {
  Run({{"apu.driver", "fake"},
       {"apu.gain", "0.5"},
       {"apu.center", "0.5"},
       {"apu.lfe", "0"},
       {"apu.surround", "2"}},
      [](rex::apu::IAudioSystem& audio) {
        auto stream = audio.OpenStream([] {});
        REQUIRE(stream);
        Feeder feeder(audio, *stream, Constant(0.2f));
        REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 30; }));
        const auto blocks = g_device.Take();
        bool found = false;
        for (size_t i = blocks.size() / 2; i < blocks.size(); ++i) {
          const Block& block = blocks[i];
          if (Peak(block, 6, 0) < 0.099f) {
            continue;
          }
          CHECK(Near(At(block, 6, 0, 100), 0.1f, 1e-4f));
          CHECK(Near(At(block, 6, 1, 100), 0.1f, 1e-4f));
          CHECK(Near(At(block, 6, 2, 100), 0.05f, 1e-4f));
          CHECK(Near(At(block, 6, 3, 100), 0.0f, 1e-6f));
          CHECK(Near(At(block, 6, 4, 100), 0.2f, 1e-4f));
          CHECK(Near(At(block, 6, 5, 100), 0.2f, 1e-4f));
          found = true;
          break;
        }
        CHECK(found);
        audio.CloseStream(*stream);
      });
}

REX_TEST_CASE("a stereo device gets a folded mix", "[apu]") {
  g_device.Reset();
  AudioApp app({{"apu.driver", "fake"}});
  g_device.stereo_only = true;
  app.body = [](rex::apu::IAudioSystem& audio) {
    CHECK(audio.GetOutputChannels() == 2);
    auto stream = audio.OpenStream([] {});
    REQUIRE(stream);
    Feeder feeder(audio, *stream, Constant(0.2f));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 30; }));
    const auto blocks = g_device.Take();
    bool found = false;
    for (size_t i = blocks.size() / 2; i < blocks.size(); ++i) {
      if (Peak(blocks[i], 2, 0) < 0.2f) {
        continue;
      }
      const rex::f32 expected = 0.2f * 2.41421356f * 0.58578644f;
      CHECK(Near(At(blocks[i], 2, 0, 100), expected, 1e-3f));
      CHECK(Near(At(blocks[i], 2, 1, 100), expected, 1e-3f));
      found = true;
      break;
    }
    CHECK(found);
    audio.CloseStream(*stream);
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("mute ramps over one period and never steps", "[apu]") {
  Run({{"apu.driver", "fake"}}, [](rex::apu::IAudioSystem& audio) {
    auto stream = audio.OpenStream([] {});
    REQUIRE(stream);
    Feeder feeder(audio, *stream, Constant(0.25f));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 20; }));
    (void)g_device.Take();

    REQUIRE(audio.GetSettings().mute.Set(true));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 10; }));
    const auto blocks = g_device.Take();

    size_t ramp = blocks.size();
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (At(blocks[i], 6, 0, 0) < 0.2495f) {
        ramp = i;
        break;
      }
    }
    REQUIRE(ramp < blocks.size());
    const Block& block = blocks[ramp];
    const rex::f32 step = 0.25f / static_cast<rex::f32>(rex::apu::kAudioChannelSamples);
    for (rex::u32 n = 1; n < rex::apu::kAudioChannelSamples; ++n) {
      const rex::f32 delta = At(block, 6, 0, n - 1) - At(block, 6, 0, n);
      CHECK(delta >= -1e-6f);
      CHECK(delta <= step * 1.01f);
    }
    CHECK(Near(At(block, 6, 0, rex::apu::kAudioChannelSamples - 1), 0.0f, 1e-6f));
    for (size_t i = ramp + 1; i < blocks.size(); ++i) {
      CHECK(Peak(blocks[i], 6, 0) == 0.0f);
    }
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("an unfocused window mutes only when asked", "[apu]") {
  g_device.Reset();
  AudioApp app({{"apu.driver", "fake"}, {"apu.mute_unfocused", "true"}});
  app.windowed = true;
  static_cast<rex::test_ui::FakeUIData&>(rex::test_ui::g_ui) = rex::test_ui::FakeUIData{};
  REQUIRE(app.Start());
  rex::apu::IAudioSystem& audio = app.GetAudio();
  auto stream = audio.OpenStream([] {});
  REQUIRE(stream);
  {
    Feeder feeder(audio, *stream, Constant(0.25f));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 20; }));

    rex::test_ui::DeliverFocus(false);
    app.Pump();
    (void)g_device.Take();
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 10; }));
    auto blocks = g_device.Take();
    CHECK(Peak(blocks.back(), 6, 0) == 0.0f);

    rex::test_ui::DeliverFocus(true);
    app.Pump();
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 10; }));
    blocks = g_device.Take();
    CHECK(Near(Peak(blocks.back(), 6, 0), 0.25f, 1e-4f));
  }
  audio.CloseStream(*stream);
  app.Finish();
}

REX_TEST_CASE("lost device keeps crediting", "[apu]") {
  Run({{"apu.driver", "fake"}}, [](rex::apu::IAudioSystem& audio) {
    CHECK(audio.GetOutputChannels() == 6);
    std::atomic<int> played{0};
    auto stream = audio.OpenStream([&played] { ++played; });
    REQUIRE(stream);
    Feeder feeder(audio, *stream, Constant(0.25f));
    REQUIRE(WaitFor([&] { return played >= 10; }));

    g_device.lost = true;
    REQUIRE(WaitFor([&] { return g_device.closes >= 1; }));
    CHECK(WaitFor([&] { return audio.GetOutputChannels() == 2; }));
    const int before = played;
    CHECK(WaitFor([&] { return played >= before + 20; }));

    g_device.lost = false;
    CHECK(WaitFor([&] { return audio.GetOutputChannels() == 6; }, 4000));
    (void)g_device.Take();
    CHECK(WaitFor([&] { return g_device.GetBlockCount() >= 5; }));
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("not finite silences at once", "[apu]") {
  Run({{"apu.driver", "fake"}}, [](rex::apu::IAudioSystem& audio) {
    std::atomic<int> played{0};
    auto stream = audio.OpenStream([&played] { ++played; });
    REQUIRE(stream);
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 3; }));
    (void)g_device.Take();

    Frame bad = Constant(0.25f);
    bad[700] = std::nanf("");
    REQUIRE(audio.Submit(*stream, bad));
    REQUIRE(WaitFor([&] { return played >= 1; }));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 5; }));
    for (const Block& block : g_device.Take()) {
      for (rex::f32 sample : block) {
        REQUIRE(std::isfinite(sample));
        CHECK(sample == 0.0f);
      }
    }

    Frame infinite = Constant(0.25f);
    infinite[3] = INFINITY;
    REQUIRE(audio.Submit(*stream, infinite));
    REQUIRE(WaitFor([&] { return played >= 2; }));
    REQUIRE(audio.Submit(*stream, Constant(0.25f)));
    REQUIRE(WaitFor([&] { return played >= 3; }));
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 6; }));
    bool audible = false;
    for (const Block& block : g_device.Take()) {
      for (rex::f32 sample : block) {
        REQUIRE(std::isfinite(sample));
      }
      audible = audible || Peak(block, 6, 0) > 0.2f;
    }
    CHECK(audible);
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("peak guard after two periods", "[apu]") {
  Run({{"apu.driver", "fake"}, {"apu.peak_limit", "0.5"}, {"apu.max_queued_frames", "16"}},
      [](rex::apu::IAudioSystem& audio) {
        std::atomic<int> played{0};
        auto stream = audio.OpenStream([&played] { ++played; });
        REQUIRE(stream);
        REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 3; }));
        (void)g_device.Take();

        for (int i = 0; i < 8; ++i) {
          REQUIRE(audio.Submit(*stream, Constant(1.0f)));
        }
        for (int i = 0; i < 8; ++i) {
          REQUIRE(audio.Submit(*stream, Constant(0.25f)));
        }
        REQUIRE(WaitFor([&] { return played >= 16; }));
        REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 20; }));
        const auto blocks = g_device.Take();

        size_t k = 0;
        while (k < blocks.size() && Peak(blocks[k], 6, 0) < 0.5f) {
          ++k;
        }
        REQUIRE(k + 7 < blocks.size());
        CHECK(Peak(blocks[k], 6, 0) >= 0.99f);
        CHECK(At(blocks[k + 1], 6, 0, 0) > 0.9f);
        CHECK(Near(At(blocks[k + 1], 6, 0, rex::apu::kAudioChannelSamples - 1), 0.0f, 0.01f));
        for (size_t i = k + 2; i < k + 8; ++i) {
          CHECK(Peak(blocks[i], 6, 0) == 0.0f);
        }
        size_t q = k + 8;
        while (q < blocks.size() && Peak(blocks[q], 6, 0) == 0.0f) {
          ++q;
        }
        REQUIRE(q + 1 < blocks.size());
        CHECK(At(blocks[q], 6, 0, 0) < 0.01f);
        CHECK(Near(At(blocks[q], 6, 0, rex::apu::kAudioChannelSamples - 1), 0.25f, 0.01f));
        CHECK(Near(Peak(blocks[q + 1], 6, 0), 0.25f, 1e-4f));
        audio.CloseStream(*stream);
      });
}

REX_TEST_CASE("a single frame over the limit passes the guard", "[apu]") {
  rex::apu::StreamGuard guard;
  Frame loud;
  loud.fill(0.9f);
  guard.Apply(loud, 0.5);
  for (rex::f32 sample : loud) {
    REQUIRE(sample == 0.9f);
  }
}

REX_TEST_CASE("two frames in a row over the limit silence the guard", "[apu]") {
  rex::apu::StreamGuard guard;
  Frame first;
  first.fill(0.9f);
  guard.Apply(first, 0.5);
  Frame second;
  second.fill(0.9f);
  guard.Apply(second, 0.5);
  CHECK(second.front() == 0.9f);
  CHECK(second.back() < 0.01f);
  Frame third;
  third.fill(0.9f);
  guard.Apply(third, 0.5);
  for (rex::f32 sample : third) {
    REQUIRE(sample == 0.0f);
  }
}

REX_TEST_CASE("the guard recovers when a frame comes back under the limit", "[apu]") {
  rex::apu::StreamGuard guard;
  for (int i = 0; i < 3; ++i) {
    Frame loud;
    loud.fill(0.9f);
    guard.Apply(loud, 0.5);
  }
  Frame quiet;
  quiet.fill(0.25f);
  guard.Apply(quiet, 0.5);
  CHECK(quiet.front() == 0.0f);
  CHECK(Near(quiet[rex::apu::kAudioChannelSamples - 1], 0.25f, 0.01f));
  Frame next;
  next.fill(0.25f);
  guard.Apply(next, 0.5);
  for (rex::f32 sample : next) {
    REQUIRE(sample == 0.25f);
  }
}

REX_TEST_CASE("the guard ramps the gain across a frame", "[apu]") {
  rex::apu::StreamGuard guard;
  for (int i = 0; i < 2; ++i) {
    Frame loud;
    loud.fill(0.9f);
    guard.Apply(loud, 0.5);
    if (i == 1) {
      for (rex::u32 n = 1; n < rex::apu::kAudioChannelSamples; ++n) {
        REQUIRE(loud[n] < loud[n - 1]);
      }
    }
  }
}

REX_TEST_CASE("speaker channels known before launch", "[apu]") {
  Run({{"apu.driver", "fake"}},
      [](rex::apu::IAudioSystem& audio) { CHECK(audio.GetOutputChannels() == 6); });
  g_device.Reset();
  g_device.stereo_only = true;
  AudioApp app({{"apu.driver", "fake"}});
  app.body = [](rex::apu::IAudioSystem& audio) { CHECK(audio.GetOutputChannels() == 2); };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("API serves the audio system only while the app runs", "[apu]") {
  g_device.Reset();
  AudioApp app({{"apu.driver", "none"}});
  CHECK(REX_INTERFACE(rex::apu::IAudioSystem) == nullptr);
  REQUIRE(app.Start());
  CHECK(REX_INTERFACE(rex::apu::IAudioSystem) == &app.GetAudio());
  app.Finish();
  CHECK(REX_INTERFACE(rex::apu::IAudioSystem) == nullptr);
}

REX_TEST_CASE("the device gets exactly the samples it asks for", "[apu]") {
  Run({{"apu.driver", "fake"}}, [](rex::apu::IAudioSystem& audio) {
    auto stream = audio.OpenStream([] {});
    REQUIRE(stream);
    g_device.pull.SetPaused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)g_device.Take();
    g_device.pull.SetRequest(480);
    for (rex::u32 i = 0; i < 6; ++i) {
      REQUIRE(audio.Submit(*stream, Constant(0.1f * static_cast<rex::f32>(i + 1))));
    }
    g_device.pull.SetPaused(false);
    REQUIRE(WaitFor([&] { return g_device.GetBlockCount() >= 4; }));
    const auto blocks = g_device.Take();
    std::vector<rex::f32> first;
    for (const Block& block : blocks) {
      CHECK(block.size() == 480 * 6);
      for (rex::u32 n = 0; n < 480; ++n) {
        first.push_back(At(block, 6, 0, n));
      }
    }
    for (rex::u32 i = 0; i < 6; ++i) {
      for (rex::u32 n = 0; n < kPeriodFrames; ++n) {
        CHECK(Near(first[i * kPeriodFrames + n], 0.1f * static_cast<rex::f32>(i + 1), 1e-5f));
      }
    }
    CHECK(first[6 * kPeriodFrames] == 0.0f);
    audio.CloseStream(*stream);
  });
}

REX_TEST_CASE("streams are limited and queues are bounded", "[apu]") {
  Run({{"apu.driver", "none"}, {"apu.max_queued_frames", "4"}}, [](rex::apu::IAudioSystem& audio) {
    std::vector<rex::apu::AudioStreamID> streams;
    for (rex::u32 i = 0; i < rex::apu::IAudioSystem::kMaxStreams; ++i) {
      auto stream = audio.OpenStream([] {});
      REQUIRE(stream);
      streams.push_back(*stream);
    }
    CHECK_FALSE(audio.OpenStream([] {}));
    CHECK(audio.GetStats().open_streams == rex::apu::IAudioSystem::kMaxStreams);

    int refused = 0;
    for (int i = 0; i < 20; ++i) {
      if (!audio.Submit(streams[0], Constant(0.1f))) {
        ++refused;
      }
    }
    CHECK(refused > 0);

    audio.CloseStream(streams[0]);
    CHECK_FALSE(audio.Submit(streams[0], Constant(0.1f)));
    CHECK(audio.OpenStream([] {}));
    CHECK_FALSE(audio.Submit(rex::apu::AudioStreamID(99), Constant(0.1f)));
  });
}

REX_TEST_CASE("a closed stream gets no further callbacks", "[apu]") {
  Run({{"apu.driver", "none"}}, [](rex::apu::IAudioSystem& audio) {
    std::atomic<int> played{0};
    int after = 0;
    auto stream = audio.OpenStream([&played] { ++played; });
    REQUIRE(stream);
    {
      Feeder feeder(audio, *stream, Constant(0.1f));
      REQUIRE(WaitFor([&] { return played >= 3; }));
      audio.CloseStream(*stream);
      after = played;
    }
    CHECK(played == after);
  });
}

REX_TEST_CASE("the driver setting is frozen and the gain setting is not", "[apu]") {
  Run({{"apu.driver", "none"}}, [](rex::apu::IAudioSystem&) {
    CHECK(REX_SETTINGS().Find("apu.driver").value()->IsFrozen());
    CHECK_FALSE(REX_SETTINGS().Find("apu.gain").value()->IsFrozen());
  });
}
