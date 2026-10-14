/**
 * @file        gpu/xenos/null/null_graphics_system.h
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

#include "gpu/xenos/graphics_system.h"

namespace rex::gpu::null {

/** Runs the title's PM4 on the null command processor with no provider and no presenter. */
class NullGraphicsSystem final : public GraphicsSystem {
 public:
  NullGraphicsSystem();
  ~NullGraphicsSystem() override;

  std::string GetName() const override;
  [[nodiscard]] Result<void> SetupPresentation(gpu::IUIThread& ui_thread) override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::gpu::null
