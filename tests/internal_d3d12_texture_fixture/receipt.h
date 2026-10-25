#pragma once
#include "fixture.h"
#include <cstring>

namespace rex::graphics::d3d12::fixture {
struct CopyTraceScope {
  const void* cp;
  uint64_t frame, submission;
  uint32_t address = 0, length = 0;
  bool result = false;
  CopyTraceScope(const void* cp, uint64_t owned, uint64_t frame, uint64_t submission) noexcept
      : cp(cp), frame(frame), submission(submission) {
    TraceCopyBegin(cp, owned, frame, submission);
  }
  ~CopyTraceScope() { TraceCopyEnd(cp, result, frame, submission, address, length); }
};
struct ObserverTraceScope {
  const void* cp;
  bool copy, prepared, result, accepted = false;
  uint64_t observer = 0, token = 0, frame, submission;
  uint32_t address = 0, length = 0;
  ~ObserverTraceScope() {
    TraceObserver(cp, copy, prepared, observer, token, frame, submission, accepted, result, address,
                  length);
  }
};
template <typename Texture>
void TraceTexture(const void* cache, TraceKind kind, const Texture& texture, bool base = false,
                  bool mips = false, uint64_t entry_generation = 0) noexcept {
  if (!TraceEnabled())
    return;
  TextureValue value;
  value.generation = texture.content_generation();
  value.instance = texture.instance_id();
  value.dirty = texture.outdated_mask();
  const auto& key = texture.key();
  static_assert(sizeof(key) <= value.key.size());
  value.key_size = sizeof(key);
  std::memcpy(value.key.data(), &key, sizeof(key));
  value.base_address = key.base_page << 12;
  value.base_size = texture.GetGuestBaseSize();
  value.mip_address = key.mip_page << 12;
  value.mip_size = texture.GetGuestMipsSize();
  const uint64_t flags = uint64_t(key.scaled_resolve) | (uint64_t(key.dimension) << 8) |
                         (uint64_t(texture.force_load_3d_tiling()) << 16);
  TraceLoad(cache, kind, value, texture.content_generation(), base, mips, entry_generation, flags);
}
}  // namespace rex::graphics::d3d12::fixture
