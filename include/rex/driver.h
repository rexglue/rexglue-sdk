/**
 * @file        driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string_view>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {

class IDriverContext;

/** The graphics family a title needs. It names the modules rexgpu-<family>-<name>. */
enum class GraphicsFamily : u8 {
  kNone,
  kXenos,
};

/** Returns the module name text of a graphics family, empty for kNone. */
[[nodiscard]] REX_API std::string_view GetGraphicsFamilyName(GraphicsFamily family);

enum class DriverType : u8 {
  kGraphics, /**< rex::gpu::IGraphicsDriver, run by GraphicsSystem. */
  kAudio,    /**< rex::apu::IAudioDriver, run by AudioSystem. */
  kInput,    /**< rex::hid::IInputDriver, run by InputSystem. Several may be active. */
  kUI,       /**< rex::ui::IUIDriver, run by UISystem. */
};

/** The lifecycle every driver shares. The system of its driver type makes every call. */
class IDriver {
 public:
  virtual ~IDriver() = default;

  /**
   * Returns the name players select it by, such as "sdl3" or "xenos".
   */
  virtual std::string_view GetName() const = 0;

  /**
   * Called when the runtime selects this driver. Settings registered here follow the startup
   * layers.
   *
   * @return Ok, or an error that fails driver selection for the system.
   */
  [[nodiscard]] virtual Result<void> Initialize(IDriverContext& context) = 0;

  /** Stops using the driver. Called once, after the title detaches when one ran. */
  virtual void Shutdown() = 0;
};

}  // namespace rex
