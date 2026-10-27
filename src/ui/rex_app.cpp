/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation - compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <cstdlib>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/fps_counter_overlay.h>
#include <rex/ui/overlay/mod_manager_overlay.h>
#include <rex/ui/overlay/notification_toast.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/output_state.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/string/numeric.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

// This consumer-compiled source links rexruntime; internal headers are not installed.
namespace rex::kernel::xam::system_link {
void Initialize(const std::filesystem::path& config_path);
}

REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(show_fps_counter, false, "Display",
                    "FPS counter. Show guest frames per second in the top-right corner")
    .audience(rex::cvar::Audience::kPlayer);

namespace {

std::string& ProfileStorage() {
  static std::string storage;
  return storage;
}

bool& ProfileCopyFromDefaultStorage() {
  static bool storage = false;
  return storage;
}

static auto profile_registrar = rex::cvar::FlagRegistrar(
    {.name = "profile",
     .type = rex::cvar::FlagType::String,
     .category = "Runtime",
     .description = "Active host-storage profile (empty or 'default' uses established paths)",
     .setter =
         [](std::string_view value) {
           // Store the raw early value so path resolution can reject invalid IDs
           // instead of silently falling back to the default environment.
           ProfileStorage() = value;
           return true;
         },
     .getter = []() { return ProfileStorage(); },
     .command_callback = [](std::string_view) {},
     .lifecycle = rex::cvar::Lifecycle::kInitOnly,
     .default_value = ""},
    rex::cvar::Audience::kPlayer, rex::cvar::Persistence::kSessionOnly);

static auto profile_copy_registrar = rex::cvar::FlagRegistrar(
    {.name = "profile_copy_from_default",
     .type = rex::cvar::FlagType::Boolean,
     .category = "Runtime",
     .description = "Copy the default environment into a new named profile once",
     .setter =
         [](std::string_view value) {
           if (value != "true" && value != "false" && value != "1" && value != "0" &&
               value != "yes" && value != "no") {
             return false;
           }
           ProfileCopyFromDefaultStorage() = rex::string::from_string<bool>(value, false);
           return true;
         },
     .getter = []() { return ProfileCopyFromDefaultStorage() ? "true" : "false"; },
     .command_callback = [](std::string_view) {},
     .lifecycle = rex::cvar::Lifecycle::kInitOnly,
     .default_value = "false"},
    rex::cvar::Audience::kHidden, rex::cvar::Persistence::kSessionOnly);

const char* ProfileCopyResultName(rex::system::ProfileCopyResult result) {
  using rex::system::ProfileCopyResult;
  switch (result) {
    case ProfileCopyResult::kSuccess:
      return "success";
    case ProfileCopyResult::kInvalidProfile:
      return "invalid profile";
    case ProfileCopyResult::kDefaultProfile:
      return "default profile selected";
    case ProfileCopyResult::kTargetExists:
      return "target already exists";
    case ProfileCopyResult::kInvalidSpecification:
      return "invalid title copy specification";
    case ProfileCopyResult::kUnsafeSource:
      return "unsafe source path";
    case ProfileCopyResult::kDestinationCollision:
      return "destination collision";
    case ProfileCopyResult::kIoError:
      return "I/O error";
    case ProfileCopyResult::kVerificationFailed:
      return "verification failed";
  }
  return "unknown error";
}

// Create the folder and a unique probe file to check writability.
// Return an empty string on success, otherwise the reason.
std::string ProbeWritableDirectory(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return ec.message();
  }
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto probe = dir / fmt::format(".rex_write_probe_{:x}", stamp);
  {
    std::ofstream out(probe, std::ios::binary);
    if (!out) {
      return "the folder is not writable";
    }
    out << 'x';
    out.flush();
    if (!out) {
      return "the folder is not writable";
    }
  }
  std::filesystem::remove(probe, ec);
  return {};
}

}  // namespace

namespace rex {

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::filesystem::path ReXApp::GetDefaultUserDataRoot() const {
  return rex::filesystem::GetUserFolder() / GetName();
}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements(),
      [this]() { DeferWhileOverlaysAlive([this]() { achievements_overlay_.reset(); }); });
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // Base user data (B): cvar override, or platform user directory.
  std::filesystem::path base_user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    base_user_dir = user_data_cvar;
  } else {
    base_user_dir = GetDefaultUserDataRoot();
  }

  const auto profile_paths = system::ResolveProfile(base_user_dir, ProfileStorage());
  if (!profile_paths) {
    auto msg = fmt::format("Invalid or unsafe profile selection: '{}'", ProfileStorage());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  const auto& user_dir = profile_paths->active_root;

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_root_cvar = REXCVAR_GET(cache_root);
  const bool cache_root_is_default = cache_root_cvar.empty();
  if (!cache_root_cvar.empty()) {
    cache_dir = cache_root_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  PathConfig path_config{game_dir,
                         user_dir,
                         update_dir,
                         cache_dir,
                         metadata_dir,
                         exe_dir / (std::string(GetName()) + ".toml"),
                         profile_paths->base_root,
                         profile_paths->profile_id};
  OnConfigurePaths(path_config);
  if (cache_root_is_default && path_config.cache_root == cache_dir) {
    path_config.cache_root = path_config.user_data_root / "cache";
  }

  // A default-profile hook may adjust only user_data_root; keep the base root
  // (B) and active root (P) consistent. Named profiles must resolve to a child of B.
  if (path_config.profile_id.empty() &&
      path_config.base_user_data_root == profile_paths->base_root &&
      path_config.user_data_root != profile_paths->active_root) {
    path_config.base_user_data_root = path_config.user_data_root;
  }
  const auto configured_profile_paths =
      system::ResolveProfile(path_config.base_user_data_root, path_config.profile_id);
  if (!configured_profile_paths || configured_profile_paths->active_root.lexically_normal() !=
                                       path_config.user_data_root.lexically_normal()) {
    auto msg = std::string("The configured profile roots are inconsistent or unsafe.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  if (ProfileCopyFromDefaultStorage()) {
    const auto specification = GetProfileCopySpecification();
    if (!specification) {
      auto msg = std::string("This title does not provide a profile copy specification.");
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
    const auto result = system::CopyFromDefault(*configured_profile_paths, *specification);
    if (result != system::ProfileCopyResult::kSuccess) {
      auto msg = fmt::format("Could not copy the default environment into profile '{}': {}.",
                             configured_profile_paths->profile_id, ProfileCopyResultName(result));
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  // Report an unwritable logging and config root before the log sink can fail silently.
  if (const auto reason = ProbeWritableDirectory(path_config.user_data_root); !reason.empty()) {
    auto msg = fmt::format(
        "Cannot write to the user data folder '{}': {}. Choose a writable folder with "
        "--user_data_root, or fix the folder permissions.",
        path_config.user_data_root.string(), reason);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = path_config.game_data_root;
  base_user_data_root_ = path_config.base_user_data_root;
  user_data_root_ = path_config.user_data_root;
  active_profile_id_ = path_config.profile_id;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  metadata_root_ = path_config.metadata_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    // Keep logs in user data so read-only installs can start.
    log_config.log_dir = (user_data_root_ / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  // Command-line values already outrank LoadConfig. Freeze before creating the Runtime.
  OnPreSystemLinkInit();
  rex::kernel::xam::system_link::Initialize(config_path_);

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_DEBUG("Loaded config: {}", config_path_.filename().string());

  REXLOG_DEBUG("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_DEBUG("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_DEBUG("  User data:      {}", user_data_root_.string());
  }
  REXLOG_DEBUG("  Host profile:   {}", active_profile_id_.empty() ? "default" : active_profile_id_);
  if (base_user_data_root_ != user_data_root_) {
    REXLOG_DEBUG("  Shared base:    {}", base_user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_DEBUG("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_DEBUG("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_DEBUG("  Metadata root:  {}", metadata_root_.string());
  }

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  base_user_data_root_ =
      paths.base_user_data_root.empty() ? paths.user_data_root : paths.base_user_data_root;
  user_data_root_ = paths.user_data_root;
  active_profile_id_ = paths.profile_id;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;

  runtime_ = std::make_unique<rex::Runtime>(paths.game_data_root, base_user_data_root_,
                                            paths.user_data_root, paths.update_data_root,
                                            paths.cache_root, paths.metadata_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() { return !IsOverlayInputCaptured(); });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  mod_infos_ = runtime_->active_mods_info();
  mod_root_strings_.clear();
  mod_root_strings_.reserve(mod_infos_.size());
  for (const auto& mod : mod_infos_) {
    mod_root_strings_.push_back(mod.mod_root.string());
  }

  rex::system::ModHostContext context{};
  context.runtime = runtime_.get();
  context.app_context = &app_context();
  context.window = window_.get();
  context.input_system = static_cast<rex::input::InputSystem*>(runtime_->input_system());
  for (size_t i = 0; i < mod_infos_.size(); ++i) {
    const auto& mod = mod_infos_[i];
    if (mod.code.empty()) {
      continue;
    }
    context.mod_root = mod_root_strings_[i].c_str();
    context.mod_name = mod.id.c_str();
    if (auto plugin = rex::system::LoadModPlugin(mod.mod_root, mod.id, mod.code, context)) {
      mod_plugin_ids_.push_back(mod.id);
      mod_plugins_.push_back(std::move(plugin));
      runtime_->MarkModActive(mod.id, true, i);
    } else {
      runtime_->MarkModLoadFailed(mod.id, "native plugin load failed");
    }
  }
  if (imgui_drawer_) {
    for (auto& plugin : mod_plugins_) {
      plugin->OnCreateDialogs(imgui_drawer_.get());
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  config_.gpu_plugin = REXCVAR_GET(gpu_plugin);
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);
    if (!config_.graphics) {
      // Fatal by design: no silent headless fallback.
      auto msg =
          fmt::format("Failed to load GPU plugin '{}'. See log for details.", config_.gpu_plugin);
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  rex::cvar::RegisterChangeCallback("fullscreen", [this](std::string_view, std::string_view value) {
    if (window_) {
      window_->SetFullscreen(rex::string::from_string<bool>(value, false));
    }
  });
  window_->Open();

  auto* graphics_system = config_.graphics.get();
  // Report the surface size before the Runtime creates size-dependent GPU caches.
  if (graphics_system) {
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    if (window_->GetPresentationPixelSize(surface_width, surface_height)) {
      graphics_system->SetDisplaySurfaceSize(surface_width, surface_height);
    }
  }
  if (graphics_system && graphics_system->presenter()) {
    // SDK mode: the emulated-Xenos presenter drives the overlays.
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        SetupOverlays(presenter, immediate_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  } else if (!graphics_system) {
    // Detached mode: the app brings its own renderer and drives its own paint
    // loop. ReXApp owns the returned drawer via immediate_drawer_.
    immediate_drawer_ = OnCreateImmediateDrawer();
    if (immediate_drawer_) {
      SetupOverlays(/*presenter=*/nullptr, immediate_drawer_.get());
      // No window_->SetPresenter, no drawer SetPresenter: the app owns the
      // surface and the present cadence.
    }
  }

  return true;
}

void ReXApp::SetupOverlays(rex::ui::Presenter* presenter, rex::ui::ImmediateDrawer* drawer) {
  // A ShutdownOverlays before setup must not suppress the real teardown.
  overlays_shutdown_ = false;
  overlay_lifetime_ = std::make_shared<int>(0);
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); },
      [this](ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
        OnConfigureStyle(imgui_style, ui_style);
      });
  // presenter is nullptr in detached mode; ImGuiDrawer tolerates that and the
  // gated eager font upload in SetImmediateDrawer is skipped (font uploads
  // lazily on the first Draw instead).
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);
  overlay_presenter_ = presenter;
  rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
    if (debug_overlay_) {
      debug_overlay_.reset();
    } else {
      debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(),
                                                                EffectiveFrameStatsProvider());
    }
  });
  rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
    if (console_overlay_) {
      console_overlay_.reset();
    } else {
      console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
    }
  });
  rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
    if (settings_overlay_) {
      settings_overlay_.reset();
    } else {
      ui::SettingsLayout layout;
      OnConfigureSettings(layout);
      settings_overlay_ = std::make_unique<ui::SettingsDialog>(
          imgui_drawer_.get(), config_path_,
          [this]() { DeferWhileOverlaysAlive([this]() { settings_overlay_.reset(); }); },
          std::move(layout));
    }
  });
  rex::ui::RegisterBind("bind_mod_manager", "F1", "Toggle mod manager overlay", [this, drawer] {
    if (mod_manager_overlay_) {
      auto* dialog = static_cast<ui::ModManagerDialog*>(mod_manager_overlay_.get());
      if (dialog->RequestClose()) {
        mod_manager_overlay_.reset();
      }
    } else {
      mod_manager_overlay_ = std::make_unique<ui::ModManagerDialog>(
          imgui_drawer_.get(), drawer, runtime_.get(),
          [this]() { DeferWhileOverlaysAlive([this]() { mod_manager_overlay_.reset(); }); });
    }
  });
  rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements overlay", [this] {
    if (achievements_overlay_) {
      achievements_overlay_.reset();
    } else {
      achievements_overlay_ = CreateAchievementsOverlay();
    }
  });
  rex::ui::RegisterBind("bind_fps_counter", "F12", "Toggle FPS counter", [] {
    rex::cvar::SetFlagByName("show_fps_counter", REXCVAR_GET(show_fps_counter) ? "false" : "true");
  });
  // Settings and binds change the flag; the dialog follows on the UI thread.
  rex::cvar::RegisterChangeCallback("show_fps_counter", [this](std::string_view, std::string_view) {
    DeferWhileOverlaysAlive([this]() { SyncFpsCounter(); });
  });
  SyncFpsCounter();
  notification_toast_ = std::make_unique<ui::NotificationToastDialog>(imgui_drawer_.get());

  OnCreateDialogs(imgui_drawer_.get());
}

ui::DebugOverlayDialog::FrameStatsProvider ReXApp::EffectiveFrameStatsProvider() const {
  if (frame_stats_provider_) {
    return frame_stats_provider_;
  }
  if (auto* presenter = overlay_presenter_) {
    return [presenter]() { return presenter->GetGuestOutputRefreshStats(); };
  }
  return {};
}

void ReXApp::DeferWhileOverlaysAlive(std::function<void()> fn) {
  app_context().CallInUIThreadDeferred(
      [lifetime = std::weak_ptr<int>(overlay_lifetime_), fn = std::move(fn)]() {
        if (lifetime.lock()) {
          fn();
        }
      });
}

void ReXApp::SyncFpsCounter() {
  if (overlays_shutdown_ || !imgui_drawer_) {
    return;
  }
  if (!REXCVAR_GET(show_fps_counter)) {
    fps_counter_overlay_.reset();
  } else if (!fps_counter_overlay_) {
    fps_counter_overlay_ =
        std::make_unique<ui::FpsCounterDialog>(imgui_drawer_.get(), EffectiveFrameStatsProvider());
  }
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    if (!achievement_notification_) {
      achievement_notification_ =
          std::shared_ptr<ui::AchievementNotificationDialog>(CreateAchievementNotificationDialog());
    }
    if (achievement_notification_ && achievement_notification_listener_ == 0 && runtime_ &&
        runtime_->kernel_state()) {
      std::weak_ptr<ui::AchievementNotificationDialog> notification = achievement_notification_;
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [notification](const rex::system::AchievementEvent& event) {
            if (auto dialog = notification.lock()) {
              dialog->Push(event);
            }
          });
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system = runtime_->graphics_system();
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    system::DispatchModLaunch(*runtime_, mod_infos_, [this](const system::ModPackage& package) {
      const auto it = std::find(mod_plugin_ids_.begin(), mod_plugin_ids_.end(), package.id);
      if (it != mod_plugin_ids_.end())
        mod_plugins_[static_cast<size_t>(it - mod_plugin_ids_.begin())]->OnModuleLaunched();
    });

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::ShutdownModPlugins() {
  for (auto& plugin : mod_plugins_) {
    plugin->OnShutdown();
  }
  mod_plugins_.clear();
  mod_plugin_ids_.clear();
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  ShutdownModPlugins();
  // Only a detached title with no Runtime can unwind normally. A presenter-backed
  // title closed before its Runtime exists (deferred path picker) still hard-exits.
  if (!runtime_ && !config_.graphics) {
    app_context().QuitFromUIThread();
    return;
  }
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  // Guest threads are stopped, but stragglers may still poll; the hook may only
  // drain and release the title's own presentation resources.
  OnBeforeHardExit();
  // Hard-exit rather than run subsystem teardown, which can deadlock on a host
  // lock still held by a straggler TerminateTitle left running. Flush (not
  // ShutdownLogging, which frees loggers a straggler may still use); the OS
  // reclaims the rest.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
  std::_Exit(0);
}

bool ReXApp::OnCloseRequested(ui::UIEvent& e) {
  (void)e;
  return OnWindowCloseRequested();
}

void ReXApp::PublishDisplaySurfaceSize(bool minimized) {
  if (!window_) {
    return;
  }
  // The Runtime owns the graphics system once it is set up.
  if (auto* graphics_system = runtime_ ? runtime_->graphics_system() : config_.graphics.get()) {
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    if (minimized || !window_->GetPresentationPixelSize(surface_width, surface_height)) {
      surface_width = 0;
      surface_height = 0;
    }
    graphics_system->SetDisplaySurfaceSize(surface_width, surface_height);
  }
}

void ReXApp::OnResize(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  PublishDisplaySurfaceSize(window_minimized_);
  OnWindowPixelSizeChanged(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  OnWindowResized(window_->GetActualLogicalWidth(), window_->GetActualLogicalHeight());
}

void ReXApp::OnDpiChanged(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnDpiScaleChanged(float(window_->GetDpi()) / float(window_->GetMediumDpi()));
}

// Runs on the UI thread; the audio device callback reads the flag. Published
// before the title hook so a title that overrides it cannot skip the update.
void ReXApp::OnGotFocus(ui::UISetupEvent& e) {
  (void)e;
  audio::SetOutputFocused(true);
  OnWindowFocusChanged(true);
}

void ReXApp::OnLostFocus(ui::UISetupEvent& e) {
  (void)e;
  audio::SetOutputFocused(false);
  OnWindowFocusChanged(false);
}

void ReXApp::OnMinimized(ui::UIEvent& e) {
  (void)e;
  audio::SetOutputFocused(false);
  window_minimized_ = true;
  PublishDisplaySurfaceSize(true);
  OnWindowMinimized();
}

void ReXApp::OnRestored(ui::UIEvent& e) {
  (void)e;
  audio::SetOutputFocused(window_ && window_->HasFocus());
  window_minimized_ = false;
  PublishDisplaySurfaceSize(false);
  OnWindowRestored();
}

bool ReXApp::IsOverlayInputCaptured() const {
  if (!imgui_drawer_ || !imgui_drawer_->HasDialogs()) {
    return false;
  }
  const auto& io = imgui_drawer_->GetIO();
  return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void ReXApp::ShutdownOverlays() {
  ShutdownOverlays(runtime_ ? static_cast<input::InputSystem*>(runtime_->input_system()) : nullptr);
}

void ReXApp::ShutdownOverlays(input::InputSystem* input_system) {
  assert_true(app_context().IsInUIThread());
  if (overlays_shutdown_) {
    return;
  }
  overlays_shutdown_ = true;
  overlay_lifetime_.reset();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_mod_manager");
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::UnregisterBind("bind_fps_counter");
  rex::cvar::UnregisterChangeCallbacks("show_fps_counter");

  // Stop borrowed access before any dialog or drawer is destroyed.
  if (input_system) {
    input_system->SetActiveCallback(nullptr);
  }
  if (runtime_) {
    runtime_->set_imgui_drawer(nullptr);
  }

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  mod_manager_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  fps_counter_overlay_.reset();
  notification_toast_.reset();
  overlay_presenter_ = nullptr;
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // immediate_drawer_ was already unlinked from imgui_drawer_ above. Detach it
  // from its presenter so SDK mode runs OnLeavePresenter() before disposal; in
  // detached mode the drawer never had a presenter, so SetPresenter(nullptr) is
  // a no-op.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  ShutdownModPlugins();
  ShutdownOverlays();

  if (runtime_) {
    runtime_->set_display_window(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = std::move(provider);
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(EffectiveFrameStatsProvider());
  }
  if (fps_counter_overlay_) {
    fps_counter_overlay_->SetStatsProvider(EffectiveFrameStatsProvider());
  }
}

}  // namespace rex
