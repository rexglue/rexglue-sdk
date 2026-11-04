/**
 * @file        xam/xam_module.h
 * @brief       The XAM module and the settings it reads
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string_view>

#include <rex/logging/logger.h>
#include <rex/module.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/xam/language.h>
#include <rex/xam/title_lifecycle.h>

#include "xam/categories.h"
#include "xam/xam_title_lifecycle.h"
#include "xam/xam_bodies.h"

REXLOG_TABLE(kXAMLog, xam)

namespace rex::xam {

/** The kernel settings the XAM exports read. Each points into the kernel module. */
struct XAMSettings {
  Setting<bool>* headless = nullptr;    /**< System dialogs answer at once without showing. */
  Setting<u32>* user_country = nullptr; /**< Console country code. */
  Setting<u32>* license_mask = nullptr; /**< Content license mask. */
};

/** Returns the settings the XAM exports read. */
const XAMSettings& GetXAMSettings();

/** The XAM module. It serves the title events, the title managers and the language detection. */
class XAMModule final : public IModule {
 public:
  REX_MODULE_SERVES(::rex::xam::IXAMTitleLifecycle, ::rex::xam::IXAMTitleServices,
                    ::rex::xam::IXAMLanguage, ::rex::xam::IXAMExports)

  std::string_view GetName() const override;
  std::span<const InterfaceType> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;

 private:
  XAMTitleLifecycle events_;
  XAMLanguage language_;
  XAMExports exports_;
  ISettingsRegistry* settings_ = nullptr;
  ILogger* logger_ = nullptr;
};

}  // namespace rex::xam
