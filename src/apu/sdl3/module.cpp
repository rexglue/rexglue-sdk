/**
 * @file        apu/sdl3/module.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

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
  static void* GetModule() { return static_cast<IModule*>(&Instance()); }
  static void* GetBackend() { return static_cast<apu::IBackend*>(&Instance().backend_); }

  static constexpr std::array<InterfaceEntry, 2> kInterfaces = {{
      {IModule::kInterfaceName, IModule::kInterfaceVersion, &GetModule},
      {apu::IBackend::kInterfaceName, apu::IBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* Name() const override { return "rex-apu-sdl3"; }
  std::span<const char* const> RequiredInterfaces() const override {
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
  static SDL3Module& Instance() {
    static SDL3Module instance;
    return instance;
  }

  cvar::ICvarRegistry* cvars_ = nullptr;
  SDL3Backend backend_;
};

}  // namespace rex::apu::sdl3

REX_DECLARE_MODULE(rex::apu::sdl3::SDL3Module)
