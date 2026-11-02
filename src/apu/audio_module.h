/**
 * @file        apu/audio_module.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/app/app_info.h>
#include <rex/logging/log.h>
#include <rex/module.h>

#include "apu/audio_system.h"

namespace rex::apu {

/** Serves the audio system, selects its drivers at Init and stops them at Shutdown. */
class AudioModule final : public IModule {
 public:
  REX_MODULE_SERVES(::rex::apu::IAudioSystem)

  std::string_view GetName() const override { return "rexapu"; }
  std::span<const InterfaceType> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;

 private:
  AudioSystem& GetOrCreateSystem();

  const rex::AppSysRequirements* needs_ = nullptr;
  std::unique_ptr<AudioSystem> system_;
  bool retired_ = false;
  IFactory* factory_ = nullptr;
  ISettings* settings_ = nullptr;
  ILogger* logger_ = nullptr;
  IDriverContext* context_ = nullptr;
  std::vector<std::unique_ptr<IDriver>> drivers_;
};

}  // namespace rex::apu
