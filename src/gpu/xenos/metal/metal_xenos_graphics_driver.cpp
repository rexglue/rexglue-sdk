/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "gpu/xenos/metal/metal_xenos_graphics_driver.h"

#include <rex/driver_context.h>

#include "gpu/xenos/command_processor.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/ui/metal/metal_provider.h"

namespace rex::gpu::metal {

MetalProviderFactoryFn MetalProviderFactory = nullptr;

MetalXenosGraphicsDriver::MetalXenosGraphicsDriver() = default;

MetalXenosGraphicsDriver::~MetalXenosGraphicsDriver() { Shutdown(); }

Result<void> MetalXenosGraphicsDriver::Initialize(IDriverContext& context) {
  return XenosGraphicsDriverBase::Initialize(context);
}

void MetalXenosGraphicsDriver::Shutdown() { XenosGraphicsDriverBase::Shutdown(); }

void MetalXenosGraphicsDriver::CreateProvider(bool with_presentation) {
  // The provider owns the device and the queue, and is needed even without a window: the headless trace dump renders
  // offscreen and reads back through the presenter.
  // The UI layer installs the provider factory before the driver is created; naming MetalProvider here would make the GPU
  // library depend on the UI library.
  if (MetalProviderFactory) {
    provider_ = MetalProviderFactory();
  }
}

std::unique_ptr<CommandProcessor> MetalXenosGraphicsDriver::CreateCommandProcessor() {
  // TODO(migration): the command processor's constructor still takes the previous base's arguments; adapt it to
  // (this, title_info_, *physical_memory_) as VulkanCommandProcessor does once the backend is wired into this tree.
  return std::make_unique<::rex::graphics::metal::MetalCommandProcessor>(
      this, title_info_, *physical_memory_, *write_watch_, *guest_interrupts_);
}

}  // namespace rex::gpu::metal
