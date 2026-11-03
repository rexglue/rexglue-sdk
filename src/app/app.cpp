#include <rex/app/app.h>
#include <rex/paths.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <thread>

#include "gpu/graphics_control.h"
#include "kernel/kernel_control.h"
#include <rex/platform/file.h>
#include <rex/platform/process.h>
#include <rex/logging/log.h>
#include <rex/logging/logger.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_module.h>
#include <rex/app/app_info.h>
#include <rex/init.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/ui/ui_system.h>
#include <rex/string/text.h>
#include <rex/utility.h>

#include "ui/ui_control.h"
#include <rex/version.h>

#include "app/lifecycle.h"
#include "base/settings/settings_control.h"
#include "base/logger_control.h"
#include "app/command_line.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace rex::app {
namespace {

constexpr u32 kFrameWaitMs = 16;

std::string DescribeLimit(u64 value) {
  return value == std::numeric_limits<u64>::max() ? "unlimited" : std::to_string(value);
}

void LogOpenFileLimit() {
  const auto raised = platform::RaiseOpenFileLimit();
  if (!raised) {
    REXLOG_WARN(core, "cannot raise the open file limit, {}", raised.error().message);
    return;
  }
  if (raised->hard == 0) {
    return;
  }
  if (raised->after > raised->before) {
    REXLOG_INFO(core, "open file limit raised from {} to {}, hard limit {}",
                DescribeLimit(raised->before), DescribeLimit(raised->after),
                DescribeLimit(raised->hard));
  } else {
    REXLOG_INFO(core, "open file limit is {}, hard limit {}", DescribeLimit(raised->after),
                DescribeLimit(raised->hard));
  }
}

}  // namespace

namespace detail {

#if REX_PLATFORM_WIN32
CLIArgList GetWindowsCommandLine() {
  i32 count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) {
    return CLIArgList::Parse(std::span<const std::string>{});
  }
  std::vector<std::string> args;
  for (i32 index = 1; index < count; ++index) {
    args.push_back(rex::string::ToUTF8(reinterpret_cast<const char16_t*>(wide[index])));
  }
  LocalFree(static_cast<HLOCAL>(wide));
  return CLIArgList::Parse(args);
}
#endif

int ReportEntryFailure(const char* what) noexcept {
  std::fprintf(stderr, "unhandled exception, %s\n", what);
  return 1;
}

}  // namespace detail

class AppInfoModule final : public IModule {
 public:
  explicit AppInfoModule(IAppInfo& info) : info_(info) {}

  std::string_view GetName() const override { return "appinfo"; }
  [[nodiscard]] Result<void> Connect(IFactory&) override { return Ok(); }
  void Disconnect() override {}
  [[nodiscard]] Result<void> Init() override { return Ok(); }
  void Shutdown() override {}
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IAppInfo>(info_, name, version);
  }

 private:
  IAppInfo& info_;
};

struct App::State final : IAppInfo {
  enum class Stage : u8 { kNone, kStarted, kShutdown };

  std::string name;
  std::atomic<AppPhase> phase{AppPhase::kStart};
  CLIArgList args;
  std::vector<std::string> positionals;
  std::optional<platform::InstanceLock> lock;
  Stage stage = Stage::kNone;
  bool failed = false;
  bool error_reported = false;
  bool registered = false;
  bool signals = false;
  bool init_done = false;
  bool headless = false;
  bool on_init_done = false;
  i32 final_code = 0;
  AppSysRequirements needs;
  IFactory* private_host = nullptr;
  ILogger* logger = nullptr;
  rex::apu::IAudioSystem* audio = nullptr;
  rex::hid::IInputSystem* input = nullptr;
  rex::gpu::IGraphicsSystem* graphics = nullptr;
  gpu::IGraphicsControl* graphics_control = nullptr;
  rex::ui::IUISystem* ui = nullptr;
  ui::IUIControl* ui_control = nullptr;
  std::vector<IModule*> code_modules;
  AppInfoModule info_module{*this};

  const AppSysRequirements& GetRequirements() const override { return needs; }

  std::string_view GetAppName() const override { return name; }

  void DetectHeadless() {
    if (ui && ui->GetSettings().driver.Get() == "none") {
      headless = true;
    }
    const std::span<const std::string> all = args.GetAll();
    for (size_t i = 0; i < all.size(); ++i) {
      if (all[i] == "--ui.driver=none" ||
          (all[i] == "--ui.driver" && i + 1 < all.size() && all[i + 1] == "none")) {
        headless = true;
      }
    }
  }

  void ForgetSystems() {
    private_host = nullptr;
    logger = nullptr;
    audio = nullptr;
    input = nullptr;
    graphics = nullptr;
    graphics_control = nullptr;
    ui = nullptr;
    ui_control = nullptr;
  }

  static void DrainNotices(ISettingsControl& control) {
    for (const std::string& notice : control.TakeNotices()) {
      REXLOG_WARN(core, "{}", notice);
    }
  }
};

namespace {

std::filesystem::path DefaultRoot(const std::string& name) {
  const std::filesystem::path root = platform::GetAppRoot(name);
  return root.empty() ? platform::GetExecutableFolder() : root;
}

}  // namespace

App::App(std::string_view name) : SettingOwner("app"), state_(std::make_unique<State>()) {
  state_->name = name;
}

App::~App() {
  if (state_->stage == State::Stage::kStarted) {
    try {
      Shutdown();
    } catch (const std::exception& error) {
      REXLOG_ERROR(core, "Shutdown from the destructor failed, {}", error.what());
    } catch (...) {
      REXLOG_ERROR(core, "Shutdown from the destructor failed with an unknown exception");
    }
  }
  if (state_->stage != State::Stage::kNone && detail::ReleaseInitOwner(this)) {
    rex::Shutdown();
  }
}

std::string_view App::GetName() const {
  return state_->name;
}

AppPhase App::GetPhase() const {
  return state_->phase.load();
}

void App::SetPhase(AppPhase phase) {
  state_->phase = phase;
}

std::span<const std::string> App::GetArgs() const {
  return state_->positionals;
}

void App::RequestShutdown() {
  rex::app::RequestShutdown(0);
}

void App::RequestRestart() {
  rex::app::RequestRestart();
}

AppSysRequirements App::GetRequirements() const {
  return {};
}

Result<void> App::OnConfigure(Settings&) {
  return Ok();
}

std::span<const CLIAction> App::GetActions() const {
  return {};
}

Result<void> App::OnAction(const CLIActionHit&) {
  return Ok();
}

void App::OnModules(std::vector<std::string>&) {}

void App::OnRegisterModules(std::vector<IModule*>&) {}

Result<void> App::OnInit() {
  return Ok();
}

void App::OnRun() {
  while (RunFrame()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void App::OnShutdown() {}

void App::OnStop() {}

void App::OnRelease() {}

void App::ReportError(std::string_view message) {
  REXLOG_ERROR(core, "{}", message);
}

platform::CrashReporterOptions App::GetCrashOptions() {
  return {};
}

rex::ui::IUISystem* App::FindUI() {
  return state_->ui;
}

bool App::IsHeadlessRequested() const {
  return state_->headless;
}

Result<void> App::Fail(std::string_view step, Error error) {
  state_->failed = true;
  state_->DetectHeadless();
  ReportError(std::format("{} failed: {}", step, error.GetDescription()));
  return std::unexpected(std::move(error));
}

bool App::RunFrame() {
  return state_->ui_control->RunFrame(kFrameWaitMs) && !IsShutdownRequested();
}

Result<void> App::Init(const CLIArgList& args) {
  State& state = *state_;
  if (state.stage != State::Stage::kNone) {
    return Fail("Init", Error(ErrorCategory::kRuntime, "App::Init called twice"));
  }
  state.args = args;
  state.stage = State::Stage::kStarted;
  state.phase = AppPhase::kStart;
  detail::ResetAppState();
  detail::InstallSignalHandlers();
  state.signals = true;

  Settings& settings = REX_SETTINGS();
  if (auto registered = settings.Register(*this); !registered) {
    return Fail("settings", registered.error());
  }
  state.registered = true;

  rex::Config config;
  config.modules = {"rexfilesystem", "rexhid", "rexapu", "rexui", "rexgpu", "rexxam", "rexkernel"};
  OnModules(config.modules);
  OnRegisterModules(state.code_modules);
  config.code_modules.push_back(&state.info_module);
  config.code_modules.insert(config.code_modules.end(), state.code_modules.begin(),
                             state.code_modules.end());
  config.select_plugins = [this, &state, &settings](
                              IFactory& factory,
                              IFactory& private_factory) -> Result<std::vector<std::string>> {
    state.private_host = &private_factory;
    const auto failed = [&](std::string_view step, Error error) {
      state.error_reported = true;
      Result<void> reported = Fail(step, std::move(error));
      return Result<std::vector<std::string>>(std::unexpected(reported.error()));
    };
    state.input = factory.Get<rex::hid::IInputSystem>();
    state.audio = factory.Get<rex::apu::IAudioSystem>();
    state.graphics = factory.Get<rex::gpu::IGraphicsSystem>();
    state.graphics_control = private_factory.Get<gpu::IGraphicsControl>();
    state.ui = factory.Get<rex::ui::IUISystem>();
    state.ui_control = private_factory.Get<ui::IUIControl>();
    if (!state.audio || !state.graphics || !state.graphics_control || !state.ui ||
        !state.ui_control) {
      return failed("systems", Error(ErrorCategory::kModule, "a system module serves no system"));
    }
    if (!state.input && GetRequirements().input) {
      return failed("systems", Error(ErrorCategory::kModule,
                                     "the program needs input and no input module is listed"));
    }
    state.logger = factory.Get<ILogger>();
    if (auto* kernel_control = private_factory.Get<kernel::IKernelControl>()) {
      kernel_control->SetControls(private_factory);
    }
    state.ui_control->SetCloseHandler([] { rex::app::RequestShutdown(0); });
    state.ui_control->SetResizeHandler([graphics = state.graphics_control](u32 width, u32 height) {
      graphics->OnSurfaceResized(width, height);
    });
    state.graphics_control->SetUIPump([ui = state.ui_control](std::chrono::milliseconds timeout) {
      return ui->ServePosts(timeout);
    });
    ISettingsControl& settings_control = *private_factory.Get<ISettingsControl>();
    ILoggerControl& logger_control = *private_factory.Get<ILoggerControl>();
    (void)name.SetText(SettingLayer::kTitle, state.name);
    (void)root.SetText(SettingLayer::kTitle, DefaultRoot(state.name).string());
    LogOpenFileLimit();

    if (auto configured = OnConfigure(settings); !configured) {
      return failed("OnConfigure", configured.error());
    }
    const std::span<const CLIAction> actions = GetActions();
    if (auto valid = rex::app::detail::ValidateCLIActions(actions); !valid) {
      return failed("command line action", valid.error());
    }
    std::vector<CLIActionHit> hits;
    if (auto parsed = rex::app::detail::ParseCommandLine(state.args, actions, &hits); !parsed) {
      return failed("command line", parsed.error());
    }
    for (const std::string& positional : state.args.GetPositionals()) {
      state.positionals.push_back(positional);
    }
    for (const CLIActionHit& hit : hits) {
      if (auto handled = OnAction(hit); !handled) {
        return failed("command line action", handled.error());
      }
    }
    if (!single_instance.Get().empty()) {
      auto lock = platform::InstanceLock::Acquire(single_instance.Get());
      if (!lock) {
        return failed("instance lock", lock.error());
      }
      state.lock.emplace(std::move(*lock));
    }
    if (const std::filesystem::path file = REX_PATHS().config_file.Get(); !file.empty()) {
      std::error_code exists;
      if (std::filesystem::exists(file, exists)) {
        if (auto loaded = settings.Load(file, SettingLayer::kFile); !loaded) {
          return failed("config file", loaded.error());
        }
      }
    }
    settings_control.ApplyEnvironment();

    logger_control.ApplySettled();
    if (auto opened = logger_control.OpenFile(log_root.Get(), state.name); !opened) {
      REXLOG_WARN(core, "{}", opened.error().GetDescription());
    }
    State::DrainNotices(settings_control);

    if (auto crash = platform::InstallCrashReporter(GetCrashOptions()); !crash) {
      return failed("crash reporter", crash.error());
    }
    state.phase = AppPhase::kProviders;
    state.needs = GetRequirements();
    std::vector<std::string> plugins;
    const auto add_plugins = [&plugins](std::vector<std::string> modules) {
      for (std::string& module : modules) {
        plugins.push_back(std::move(module));
      }
    };
    state.DetectHeadless();
    add_plugins(GetDriverModules(DriverType::kAudio, state.audio->GetSettings().driver.Get(), {},
                                 &factory));
    if (state.input) {
      add_plugins(GetDriverModules(DriverType::kInput, state.input->GetSettings().drivers.Get(), {},
                                   &factory));
    }
    add_plugins(
        GetDriverModules(DriverType::kUI, state.ui->GetSettings().driver.Get(), {}, &factory));
    add_plugins(GetDriverModules(DriverType::kGraphics, state.graphics->GetDriverName(),
                                 state.needs.graphics_family, &factory));
    return plugins;
  };

  detail::SetInitOwner(this);
  if (auto started = rex::Init(config); !started) {
    state.ForgetSystems();
    if (state.error_reported) {
      return started;
    }
    return Fail("modules", started.error());
  }
  ISettingsControl& settings_control = *state.private_host->Get<ISettingsControl>();
  State::DrainNotices(settings_control);
  detail::SetShutdownWake([ui = state.ui_control] { ui->EndFrameLoop(); });

  settings_control.Freeze(SettingLife::kStartupOnly);
  state.phase = AppPhase::kRunning;
  if (auto initialized = OnInit(); !initialized) {
    return Fail("OnInit", initialized.error());
  }
  state.on_init_done = true;
  REX_TRY_VOID(ReportUnknownSettings());
  state.init_done = true;
  return Ok();
}

Result<void> App::ReportUnknownSettings() {
  std::string hard;
  for (const UnknownSetting& unknown :
       state_->private_host->Get<ISettingsControl>()->TakeUnknown()) {
    if (unknown.layer == SettingLayer::kCommandLine) {
      hard += std::format("{}unknown setting '{}' from CommandLine", hard.empty() ? "" : ", ",
                          unknown.name);
    } else if (unknown.layer == SettingLayer::kFile ||
               unknown.layer == SettingLayer::kEnvironment) {
      REXLOG_WARN(core, "unknown setting '{}' from {}", unknown.name,
                  unknown.layer == SettingLayer::kFile ? "Config" : "Environment");
    }
  }
  if (!hard.empty()) {
    return Fail("command line", Error(ErrorCategory::kConfig, std::move(hard)));
  }
  return Ok();
}

void App::Run() {
  if (state_->init_done) {
    OnRun();
  }
}

rex::hid::IInputSystem& App::GetInput() {
  REX_CHECK(core, state_->input, "GetInput is valid from OnConfigure, once the modules connect");
  return *state_->input;
}

rex::gpu::IGraphicsSystem& App::GetGraphics() {
  REX_CHECK(core, state_->graphics,
            "GetGraphics is valid from OnConfigure, once the modules connect");
  return *state_->graphics;
}

rex::ui::IUISystem& App::GetUI() {
  REX_CHECK(core, state_->ui, "GetUI is valid from OnConfigure, once the modules connect");
  return *state_->ui;
}

IFactory& App::GetControlFactory() {
  REX_CHECK(core, state_->private_host,
            "GetControlFactory is valid from OnConfigure, once the modules connect");
  return *state_->private_host;
}

rex::apu::IAudioSystem& App::GetAudio() {
  REX_CHECK(core, state_->audio, "GetAudio is valid from OnConfigure, once the modules connect");
  return *state_->audio;
}

int App::Shutdown() {
  State& state = *state_;
  if (state.stage == State::Stage::kShutdown) {
    return state.final_code;
  }
  if (state.stage == State::Stage::kNone) {
    state.stage = State::Stage::kShutdown;
    state.phase = AppPhase::kShutdown;
    state.final_code = state.failed ? 1 : 0;
    return state.final_code;
  }
  state.phase = AppPhase::kShutdown;
  detail::SetShutdownWake({});
  if (state.ui_control) {
    state.ui_control->SetCloseHandler({});
    state.ui_control->SetResizeHandler({});
    state.ui_control->BeginStop();
  }
  OnStop();
  if (state.on_init_done) {
    OnShutdown();
  }
  OnRelease();
  if (state.registered) {
    REX_SETTINGS().Unregister(*this);
    state.registered = false;
  }
  state.lock.reset();
  rex::Stop();

  if (auto requested = detail::GetRequestedExitCode()) {
    state.final_code = *requested;
  } else if (state.failed) {
    state.final_code = 1;
  } else {
    state.final_code = 0;
  }
  if (auto restart = detail::TakeRestartArgs(); restart && detail::CanRelaunchProcess()) {
    std::vector<std::string> relaunch_args = *restart;
    if (relaunch_args.empty()) {
      relaunch_args.assign(state.args.GetAll().begin(), state.args.GetAll().end());
    }
    if (auto relaunched = detail::RelaunchProcess(relaunch_args); !relaunched) {
      REXLOG_ERROR(core, "relaunch failed: {}", relaunched.error().GetDescription());
    }
  }
  if (state.logger) {
    state.logger->Flush();
  }
  if (state.signals) {
    detail::RemoveSignalHandlers();
    state.signals = false;
  }
  state.stage = State::Stage::kShutdown;
  detail::FinishTeardown();
  return state.final_code;
}

}  // namespace rex::app
