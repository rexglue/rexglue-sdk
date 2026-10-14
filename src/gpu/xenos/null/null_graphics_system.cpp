#include "gpu/xenos/null/null_graphics_system.h"

#include "gpu/xenos/null/null_command_processor.h"

namespace rex::gpu::null {

NullGraphicsSystem::NullGraphicsSystem() = default;

NullGraphicsSystem::~NullGraphicsSystem() = default;

std::string NullGraphicsSystem::GetName() const {
  return "null";
}

Result<void> NullGraphicsSystem::SetupPresentation(gpu::IUIThread&) {
  return Ok();
}

void NullGraphicsSystem::CreateProvider(bool) {}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, *runtime_);
}

}  // namespace rex::gpu::null
