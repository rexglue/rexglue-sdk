/**
 * @file        xbdm/xbdm_module.h
 * @brief       The XBDM module
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include <rex/module.h>

#include "xbdm/xbdm_bodies.h"

namespace rex::xbdm {

/** The XBDM module. It serves the bodies of the XBDM exports. */
class XBDMModule final : public IModule {
 public:
  REX_MODULE_SERVES(::rex::xbdm::IXBDMExports)

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;

 private:
  XBDMExports exports_;
};

}  // namespace rex::xbdm
