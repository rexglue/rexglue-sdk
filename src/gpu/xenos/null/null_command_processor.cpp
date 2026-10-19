#include "gpu/xenos/null/null_command_processor.h"

#include <rex/hash.h>

namespace rex::gpu::null {

NullCommandProcessor::NullCommandProcessor(GraphicsSystem* graphics_system, Runtime& runtime)
    : CommandProcessor(graphics_system, runtime) {}

NullCommandProcessor::~NullCommandProcessor() = default;

void NullCommandProcessor::IssueSwap(u32, u32, u32) {}

bool NullCommandProcessor::SetupContext() {
  return true;
}

void NullCommandProcessor::ShutdownContext() {
  shaders_.clear();
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type, u32,
                                         const u32* host_address, u32 dword_count) {
  const u64 hash = XXH3_64bits(host_address, dword_count * sizeof(u32));
  auto [it, inserted] = shaders_.try_emplace(hash);
  if (inserted) {
    it->second = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  }
  return it->second.get();
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType, u32, IndexBuffer*, bool) {
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  return true;
}

}  // namespace rex::gpu::null
