#include "gpu/xenos/shared_cvars.h"

#include "gpu/xenos/cvar_table_xenos.h"

namespace rex::gpu {

std::string GetDumpShaders() {
  return REXCVAR_GET(::rex::gpu::cvars::dump_shaders);
}

bool GetAllowInvalidFetchConstants() {
  return REXCVAR_GET(::rex::gpu::cvars::allow_invalid_fetch_constants);
}

i32 GetQueryOcclusionFakeSampleCount() {
  return REXCVAR_GET(::rex::gpu::cvars::query_occlusion_fake_sample_count);
}

}  // namespace rex::gpu
