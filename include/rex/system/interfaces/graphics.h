/**
 * @file        system/interfaces/graphics.h
 * @brief       Abstract graphics system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/system/xtypes.h>

// Forward declarations
namespace rex::runtime {
class Processor;
}
namespace rex::ui {
class WindowedAppContext;
}
namespace rex::system {
class KernelState;
}

namespace rex::system {

class IGraphicsSystem {
 public:
  virtual ~IGraphicsSystem() = default;
  virtual X_STATUS Setup(runtime::Processor* processor, KernelState* kernel_state,
                         ui::WindowedAppContext* app_context, bool with_presentation) = 0;
  virtual void Shutdown() = 0;
};

}  // namespace rex::system
