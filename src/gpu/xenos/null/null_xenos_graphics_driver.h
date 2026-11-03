/**
 * @file        gpu/xenos/null/null_xenos_graphics_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::null {

/** Runs the title's PM4 on the null command processor with no provider and no presenter. */
class NullXenosGraphicsDriver final : public XenosGraphicsDriverBase {
 public:
  NullXenosGraphicsDriver();
  ~NullXenosGraphicsDriver() override;

  std::string_view GetName() const override { return "null"; }

  /** Accepts the window and builds no presenter, so the driver presents nothing. */
  [[nodiscard]] Result<void> SetSurface(const NativeWindow& window) override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::gpu::null
