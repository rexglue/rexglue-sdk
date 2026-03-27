/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <imgui.h>

#include <filesystem>

REXCVAR_DEFINE_STRING(user_data_root, "", "Runtime", "Override user data path");
REXCVAR_DEFINE_STRING(update_data_root, "", "Runtime", "Override update data path");

namespace rex {

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {
  AddPositionalOption("game_directory");
}

bool ReXApp::OnInitialize() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  // Game directory: positional arg or default to exe_dir/assets
  std::filesystem::path game_dir;
  if (auto arg = GetArgument("game_directory")) {
    game_dir = *arg;
  } else {
    game_dir = exe_dir / "assets";
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Allow subclass to override path defaults
  PathConfig path_config{game_dir, user_dir, update_dir};
  OnConfigurePaths(path_config);
  game_data_root_ = std::move(path_config.game_data_root);
  user_data_root_ = std::move(path_config.user_data_root);
  update_data_root_ = std::move(path_config.update_data_root);

  // Logging setup from CVARs
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
    log_level_str = "trace";
  }
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, {});
  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  // Attach log capture sink to all loggers for the console overlay
  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  // Load saved config (CVARs) before anything reads them
  auto config_path = exe_dir / (std::string(GetName()) + ".toml");
  if (std::filesystem::exists(config_path)) {
    rex::cvar::LoadConfig(config_path);
    REXLOG_INFO("Loaded config: {}", config_path.filename().string());
  }

  REXLOG_INFO("{} starting", GetName());
  REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }

  // Create runtime
  runtime_ = std::make_unique<rex::Runtime>(game_data_root_, user_data_root_, update_data_root_);
  runtime_->set_app_context(&app_context());

  // Build runtime config with default platform backends
  rex::RuntimeConfig config;
#if REX_HAS_D3D12
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#elif REX_HAS_VULKAN
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#endif
  config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config.kernel_init = rex::kernel::InitializeKernel;

  // Allow subclass to customize config
  OnPreSetup(config);

  auto status = runtime_->Setup(ppc_info_.code_base, ppc_info_.code_size, ppc_info_.image_base,
                                ppc_info_.image_size, ppc_info_.func_mappings, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  std::string xex_image = "game:\\default.xex";

  // Allow subclass to override xex image
  OnLoadXexImage(xex_image);

  // Load XEX image
  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load XEX: {:08X}", status);
    return false;
  }

  // Initialize rexcrt heap after LoadXexImage to avoid guest memory writes
  // corrupting the heap region. rexcrt_heap is set by codegen (REXCRT_HEAP)
  // when [rexcrt] contains heap functions -- originals are stripped so init
  // is required. Size is controlled by the rexcrt_heap_size_mb CVAR.
  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  // Notify subclass
  OnPostSetup();

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

  // Attach window to input system so deferred drivers (e.g. MnK) can register
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  // Always expose the window to the runtime so hooks (native rendering, etc.)
  // can obtain the native handle even when the SDK graphics system is disabled.
  runtime_->set_display_window(window_.get());

  // Setup graphics presenter and ImGui
  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        // Built-in overlays
        debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get());
        console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
        settings_overlay_ = std::make_unique<ui::SettingsDialog>(
            imgui_drawer_.get(), exe_dir / (std::string(GetName()) + ".toml"));

        // Allow subclass to add custom dialogs
        OnCreateDialogs(imgui_drawer_.get());

        runtime_->set_imgui_drawer(imgui_drawer_.get());

        // Tell input drivers to suppress input when ImGui wants the mouse
        // (e.g. overlay is open). This controls MnK mouse capture.
        auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
        if (input_sys) {
          input_sys->SetActiveCallback([]() { return !ImGui::GetIO().WantCaptureMouse; });
        }
      }
    }
    window_->SetPresenter(presenter);
  }

  // Launch module in background
  app_context().CallInUIThreadDeferred([this]() {
    auto main_thread = runtime_->LaunchModule();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });

  return true;
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // ImGui cleanup (reverse of setup)
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
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
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(std::move(provider));
  }
}

}  // namespace rex
