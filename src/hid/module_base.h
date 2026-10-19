/**
 * @file        hid/module_base.h
 * @brief       The lifecycle every hid module shares
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

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/module.h>

#include "hid/hub.h"

namespace rex::hid {

/**
 * Adds the hid cvars at Connect, picks the assignment from Config::hid and adds the module's pad
 * driver at Init. The hub exists from construction, so the HID backend interface is served before
 * Init.
 */
class HIDModuleBase : public IModule {
 public:
  std::span<const char* const> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory* factory, const Config& config) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;

  /**
   * Returns the hub the module serves.
   *
   * @return The hub.
   */
  Hub& GetHub() { return hub_; }

 protected:
  /**
   * Builds and sets up the module's pad driver.
   *
   * @param config The hid config.
   * @return The driver, or the reason it is unavailable.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<Driver>> CreatePadDriver(
      const HIDConfig& config) = 0;

 private:
  cvar::ICvarRegistry* cvars_ = nullptr;
  HIDConfig config_;
  Hub hub_;
};

}  // namespace rex::hid
