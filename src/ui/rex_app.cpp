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
#include <rex/filesystem.h>
#include <rex/logging.h>
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
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>

#include <imgui.h>

#include <filesystem>

// These are defined by the generated *_config.h / *_init.h headers
// that the consumer includes before rex_app.h in their main.cpp.
extern const uint64_t PPC_CODE_BASE;
extern const uint64_t PPC_CODE_SIZE;
extern const uint64_t PPC_IMAGE_BASE;
extern const uint64_t PPC_IMAGE_SIZE;
extern const PPCFuncMapping PPCFuncMappings[];

namespace rex {

// --- DebugOverlayDialog ---

DebugOverlayDialog::DebugOverlayDialog(ui::ImGuiDrawer* imgui_drawer)
    : ImGuiDialog(imgui_drawer) {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.5f);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
  }
  ImGui::End();
}

// --- ReXApp ---

std::unique_ptr<ui::WindowedApp> ReXApp::Create(ui::WindowedAppContext& ctx) {
  // Note: subclasses that need custom construction should override Create().
  // This default creates the base ReXApp — but typically the consumer's
  // subclass inherits the constructor via `using rex::ReXApp::ReXApp;`
  // and the generated code uses SubclassName::Create instead.
  return std::make_unique<ReXApp>(ctx);
}

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, std::string_view usage)
    : WindowedApp(ctx, name, usage) {
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

  // Logging setup from CVARs
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
    log_level_str = "trace";
  }
  auto log_config = rex::BuildLogConfig(
      log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(), log_level_str, {});
  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  REXLOG_INFO("{} starting", GetName());
  REXLOG_INFO("  Game directory: {}", game_dir.string());

  // Create runtime
  runtime_ = std::make_unique<rex::Runtime>(game_dir);
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

  // Allow subclass to customize config
  OnPreSetup(config);

  auto status = runtime_->Setup(
      static_cast<uint32_t>(PPC_CODE_BASE), static_cast<uint32_t>(PPC_CODE_SIZE),
      static_cast<uint32_t>(PPC_IMAGE_BASE), static_cast<uint32_t>(PPC_IMAGE_SIZE),
      PPCFuncMappings, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  // Load XEX image
  status = runtime_->LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load XEX: {:08X}", status);
    return false;
  }

  // Notify subclass
  OnPostSetup();

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  window_->AddListener(this);
  window_->Open();

  // Setup graphics presenter and ImGui
  auto* graphics_system =
      static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        debug_overlay_ =
            std::unique_ptr<DebugOverlayDialog>(new DebugOverlayDialog(imgui_drawer_.get()));

        // Allow subclass to add custom dialogs
        OnCreateDialogs(imgui_drawer_.get());

        runtime_->set_display_window(window_.get());
        runtime_->set_imgui_drawer(imgui_drawer_.get());
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
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

}  // namespace rex
