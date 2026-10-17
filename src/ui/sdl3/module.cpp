#include <array>
#include <span>
#include <string>
#include <string_view>

#include <SDL3/SDL_hints.h>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/gpu.h>
#include <rex/hid.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/module.h>
#include <rex/runtime/achievement_manager.h>

#include "ui/sdl3/cvar_table.h"
#include "ui/sdl3/user_interface.h"

namespace rex::ui::sdl3 {

namespace {

struct ScopeTable {
  std::string_view scope;
  std::span<const cvar::VarBase* const> vars;
};

const std::array<ScopeTable, 3> kScopes = {{
    {"ui", rex::ui::cvars::kUICvars},
    {"console", rex::ui::cvars::kConsoleCvars},
    {"bind", {}},
}};

}  // namespace

class UIModule final : public IModule {
 public:
  static void* GetModule() { return static_cast<IModule*>(&GetInstance()); }
  static void* GetUI() { return static_cast<IUserInterface*>(&GetInstance().ui_); }

  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {IUserInterface::kInterfaceName, IUserInterface::kInterfaceVersion, &GetUI},
  }};

  const char* GetName() const override { return "rexui-sdl3"; }

  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }

  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config& config) override {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    REX_TRY(cvars_, factory->Get<cvar::ICvarRegistry>());
    registered_ = 0;
    for (const ScopeTable& table : kScopes) {
      if (auto status = cvars_->Register(table.scope, table.vars); !status) {
        Disconnect();
        return status;
      }
      ++registered_;
    }
    const auto& window = config.ui.window;
    auto defaults = [&]() -> Result<void> {
      REX_TRY_VOID(cvars_->SetDefault("ui.window_width", std::to_string(window.width)));
      REX_TRY_VOID(cvars_->SetDefault("ui.window_height", std::to_string(window.height)));
      REX_TRY_VOID(cvars_->SetDefault("ui.fullscreen", window.fullscreen ? "true" : "false"));
      REX_TRY_VOID(cvars_->SetDefault("ui.fullscreen_exclusive",
                                      window.fullscreen_exclusive ? "true" : "false"));
      REX_TRY_VOID(cvars_->SetDefault("ui.monitor", std::to_string(window.monitor)));
      return Ok();
    };
    if (auto status = defaults(); !status) {
      Disconnect();
      return status;
    }
    hid::IHIDBackend* hid = nullptr;
    if (auto backend = factory->Get<hid::IHIDBackend>(); backend) {
      hid = *backend;
    }
    gpu::IGPUBackend* gpu = nullptr;
    if (auto backend = factory->Get<gpu::IGPUBackend>(); backend) {
      gpu = *backend;
    }
    runtime::IAchievementManager* achievements = nullptr;
    if (auto manager = factory->Get<runtime::IAchievementManager>(); manager) {
      achievements = *manager;
    }
    ILogger* logger = nullptr;
    if (auto found = factory->Get<ILogger>(); found) {
      logger = *found;
    }
    ui_.Connect(config, *cvars_, hid, gpu, achievements, logger);
    return Ok();
  }

  void Disconnect() override {
    ui_.Disconnect();
    if (cvars_) {
      for (size_t i = registered_; i > 0; --i) {
        cvars_->Unregister(kScopes[i - 1].vars);
      }
    }
    registered_ = 0;
    cvars_ = nullptr;
  }

  [[nodiscard]] Result<void> Init() override { return Ok(); }

  void Shutdown() override { ui_.CloseWindow(); }

 private:
  static UIModule& GetInstance() {
    static UIModule instance;
    return instance;
  }

  cvar::ICvarRegistry* cvars_ = nullptr;
  size_t registered_ = 0;
  SDL3UserInterface ui_;
};

}  // namespace rex::ui::sdl3

REX_DECLARE_MODULE(rex::ui::sdl3::UIModule)
