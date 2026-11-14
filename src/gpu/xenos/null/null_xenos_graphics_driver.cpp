#include "gpu/xenos/null/null_xenos_graphics_driver.h"

#include "gpu/xenos/null/null_command_processor.h"

namespace rex::gpu::null {

NullXenosGraphicsDriver::NullXenosGraphicsDriver() = default;

NullXenosGraphicsDriver::~NullXenosGraphicsDriver() = default;

Result<void> NullXenosGraphicsDriver::SetSurface(const NativeWindow&) {
  return Ok();
}

void NullXenosGraphicsDriver::CreateProvider(bool) {}

std::unique_ptr<CommandProcessor> NullXenosGraphicsDriver::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, title_info_, *physical_memory_, *write_watch_,
                                                *guest_interrupts_);
}

}  // namespace rex::gpu::null
