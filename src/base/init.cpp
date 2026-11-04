#include <rex/init.h>

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/memory/host.h>
#include <rex/platform/perf.h>
#include <rex/platform/process.h>
#include <rex/platform/timer.h>
#include <rex/settings_registry.h>
#include <rex/driver_context.h>
#include <rex/ui/ui_thread.h>

#include "base/app_state.h"
#include "base/driver_context.h"
#include "base/core_settings.h"
#include "base/module_group.h"
#include "base/process_control.h"

namespace rex {

namespace {

struct InitState {
  std::unique_ptr<ModuleGroup> group;
  std::unique_ptr<AppDriverContext> context;
  ScopeGuard alloc_stack_depth_watch;
  bool live = false;
  bool timer_resolution = false;
  bool stopped = false;
};

InitState g_state;

void StopModules() {
  detail::SetAPIFactory(nullptr);
  if (g_state.group) {
    g_state.group->Disconnect();
    g_state.group.reset();
  }
  g_state.context.reset();
}

Result<void> Unwind(Error error) {
  StopModules();
  platform::detail::ShutdownTimerQueue();
  return std::unexpected(std::move(error));
}

}  // namespace

Result<void> Init(const Config& config) {
  if (g_state.live && g_state.stopped) {
    Shutdown();
  }
  if (g_state.live) {
    return Err(ErrorCategory::kRuntime, "rex::Init called twice");
  }
  if (auto crash = platform::InstallCrashReporter({}); !crash) {
    return crash;
  }
  g_state.live = true;
  g_state.stopped = false;
  detail::RegisterCoreSettings();
  (void)REX_LOGGER();
  PerfSettings& perf = GetPerfSettings();
  g_state.alloc_stack_depth_watch =
      perf.alloc_stack_depth.OnChange([](const i32& depth) { memory::SetAllocStackDepth(depth); });
  memory::SetAllocStackDepth(perf.alloc_stack_depth.Get());

  g_state.group = std::make_unique<ModuleGroup>();
  g_state.context = std::make_unique<AppDriverContext>(REX_SETTINGS(), REX_LOGGER());
  ModuleGroup& group = *g_state.group;
  group.Serve(static_cast<IDriverContext&>(*g_state.context), IDriverContext::kInterface);
  group.Observe([context = g_state.context.get()](IModule& module, bool inited) {
    auto* ui = static_cast<IUIThread*>(
        module.QueryInterface(IUIThread::kInterface.name, IUIThread::kInterface.version));
    if (ui) {
      context->SetUI(inited ? ui : nullptr);
    }
  });

  if (auto added = group.Add(config.code_modules); !added) {
    return Unwind(added.error());
  }
  for (const std::string& name : config.modules) {
    if (auto loaded = group.Load(name); !loaded) {
      return Unwind(loaded.error());
    }
  }
  if (auto connected = group.Connect(); !connected) {
    return Unwind(connected.error());
  }

  std::vector<std::string> plugins;
  if (config.select_plugins) {
    auto selected = config.select_plugins(group, group.GetPrivateFactory());
    if (!selected) {
      return Unwind(selected.error());
    }
    plugins = std::move(*selected);
  }
  std::string load_errors;
  for (const std::string& plugin : plugins) {
    if (auto loaded = group.Load(plugin); !loaded) {
      REXLOG_WARN("driver module '{}': {}", plugin, loaded.error().message);
      load_errors += std::format("; driver module '{}': {}", plugin, loaded.error().message);
    }
  }

  detail::SetAPIFactory(&group);
  if (GetPerfSettings().profiler.Get()) {
    platform::Profiler::Startup();
  }
  detail::BeginTimerResolution();
  g_state.timer_resolution = true;

  if (auto started = group.Init(); !started) {
    Error error = started.error();
    error.message += load_errors;
    return Unwind(std::move(error));
  }
  return Ok();
}

void Stop() {
  if (!g_state.live || !g_state.group || g_state.stopped) {
    return;
  }
  g_state.group->Disconnect();
  detail::SetAPIFactory(nullptr);
  platform::detail::ShutdownTimerQueue();
  g_state.stopped = true;
}

void Shutdown() {
  if (!g_state.live) {
    return;
  }
  Stop();
  platform::Profiler::Shutdown();
  if (g_state.timer_resolution) {
    detail::EndTimerResolution();
    g_state.timer_resolution = false;
  }
  platform::detail::ShutdownTimerQueue();
  StopModules();
  g_state.alloc_stack_depth_watch.Reset();
  platform::UninstallCrashReporter();
  for (const std::string& notice : GetProcessSettingsControl().TakeNotices()) {
    REXLOG_WARN("{}", notice);
  }
  REX_LOGGER().Flush();
  GetProcessSettingsControl().EndSession();
  GetProcessLoggerControl().EndSession();
  g_state.live = false;
  g_state.stopped = false;
}

}  // namespace rex
