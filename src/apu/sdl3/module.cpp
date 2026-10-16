#include <array>
#include <span>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/module.h>

#include <SDL3/SDL_hints.h>

#include "apu/sdl3/cvar_table.h"
#include "apu/sdl3/sdl3_backend.h"

namespace rex::apu::sdl3 {

class SDL3Module final : public IModule {
 public:
  static void* GetModule() { return static_cast<IModule*>(&GetInstance()); }
  static void* GetBackend() { return static_cast<apu::IAPUBackend*>(&GetInstance().backend_); }

  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {apu::IAPUBackend::kInterfaceName, apu::IAPUBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* GetName() const override { return "rexapu-sdl3"; }
  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }

  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config&) override {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    REX_TRY(cvars_, factory->Get<cvar::ICvarRegistry>());
    if (auto added = cvars_->Add(cvars::kSDL3Cvars); !added) {
      cvars_ = nullptr;
      return added;
    }
    return Ok();
  }

  void Disconnect() override {
    if (cvars_) {
      cvars_->Unregister(cvars::kSDL3Cvars);
    }
    cvars_ = nullptr;
  }

  [[nodiscard]] Result<void> Init() override { return backend_.Init(); }

  void Shutdown() override { backend_.Shutdown(); }

 private:
  static SDL3Module& GetInstance() {
    static SDL3Module instance;
    return instance;
  }

  cvar::ICvarRegistry* cvars_ = nullptr;
  SDL3Backend backend_;
};

}  // namespace rex::apu::sdl3

REX_DECLARE_MODULE(rex::apu::sdl3::SDL3Module)
