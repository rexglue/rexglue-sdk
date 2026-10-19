/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdint>
#include <utility>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include "gpu/xenos/pipeline/texture/cache.h"
#include <rex/xenos/texture_info.h>
#include <rex/xenos/texture_layout.h>
#include <rex/xenos/register_file.h>
#include <rex/xenos/xenos.h>
#include <rex/logging/log.h>
#include <rex/math.h>

#include "gpu/xenos/cvar_table_gpu.h"
#include "gpu/xenos/cvar_table_xenos.h"

namespace rex::gpu {

// Order matches LoadShaderIndex.
const TextureCache::LoadShaderInfo TextureCache::kLoadShaderInfo[kLoadShaderCount] = {
    {3, 4, 1, 4}, {4, 4, 2, 4}, {4, 4, 4, 3}, {4, 4, 8, 2}, {4, 4, 16, 1}, {4, 4, 2, 4},
    {4, 4, 2, 4}, {4, 4, 4, 4}, {4, 4, 2, 4}, {4, 4, 2, 4}, {4, 4, 2, 4},  {4, 4, 4, 4},
    {4, 4, 4, 3}, {4, 4, 8, 3}, {4, 4, 4, 3}, {4, 4, 8, 3}, {4, 4, 8, 3},  {4, 4, 8, 3},
    {4, 4, 8, 3}, {4, 4, 8, 3}, {4, 4, 2, 4}, {4, 4, 2, 4}, {4, 4, 4, 3},  {4, 4, 4, 3},
    {4, 4, 8, 2}, {4, 4, 8, 2}, {4, 4, 4, 2}, {4, 4, 4, 1}, {4, 4, 4, 1},  {4, 4, 2, 1},
    {4, 4, 1, 2}, {4, 4, 2, 2}, {4, 4, 2, 2}, {4, 4, 1, 2}, {4, 4, 2, 2},  {4, 4, 4, 3},
    {4, 4, 4, 3},
};

TextureCache::TextureCache(const xenos::RegisterFile& register_file, SharedMemory& shared_memory,
                           u32 draw_resolution_scale_x, u32 draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {
  REX_ASSERT(draw_resolution_scale_x >= 1);
  REX_ASSERT(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  REX_ASSERT(draw_resolution_scale_y >= 1);
  REX_ASSERT(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr u32 kScaledResolvePageDwordCount = SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ = std::make_unique<u32[]>(kScaledResolvePageDwordCount);
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ =
        shared_memory.RegisterGlobalWatch(ScaledResolveGlobalWatchCallbackThunk, this);
  }
}

TextureCache::~TextureCache() {
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    GetSharedMemory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
}

bool TextureCache::GetConfigDrawResolutionScale(u32& x_out, u32& y_out) {
  u32 config_x =
      static_cast<u32>(std::max(INT32_C(1), REXCVAR_GET(::rex::gpu::cvars::resolution_scale_x)));
  u32 config_y =
      static_cast<u32>(std::max(INT32_C(1), REXCVAR_GET(::rex::gpu::cvars::resolution_scale_y)));
  u32 clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  u32 clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

void TextureCache::ClearCache() {
  DestroyAllTextures();
}

void TextureCache::CompletedSubmissionUpdated(u64 completed_submission_index) {
  u64 current_time = rex::Clock::QueryHostUptimeMillis();
  // The render to texture limit is included in the soft and hard limits at 1x.
  u32 limit_scaled_resolve_add_mb =
      REXCVAR_GET(::rex::gpu::cvars::texture_cache_memory_limit_render_to_texture) *
      (GetDrawResolutionScaleX() * GetDrawResolutionScaleY() - 1);
  u32 limit_soft_mb =
      REXCVAR_GET(::rex::gpu::cvars::texture_cache_memory_limit_soft) + limit_scaled_resolve_add_mb;
  u32 limit_hard_mb =
      REXCVAR_GET(::rex::gpu::cvars::texture_cache_memory_limit_hard) + limit_scaled_resolve_add_mb;
  u32 limit_soft_lifetime =
      REXCVAR_GET(::rex::gpu::cvars::texture_cache_memory_limit_soft_lifetime) * 1000;
  bool destroyed_any = false;
  while (texture_used_first_ != nullptr) {
    u64 total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->GetLastUsageSubmissionIndex() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded &&
        (texture->GetLastUsageTime() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // Bindings may reference the destroyed texture, so reset them.
      ResetTextureBindings();
    }
    auto found_texture_it = textures_.find(texture->GetKey());
    REX_ASSERT(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      REX_ASSERT(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
    }
  }
  if (destroyed_any) {
    REX_PROFILE_PLOT("gpu/texture_cache/textures", textures_.size());
  }
}

void TextureCache::BeginSubmission(u64 new_submission_index) {
  REX_ASSERT(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = rex::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  ResetTextureBindings();
}

void TextureCache::MarkRangeAsResolved(u32 start_unscaled, u32 length_unscaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (IsDrawResolutionScaled()) {
    u32 page_first = start_unscaled >> 12;
    u32 page_last = (start_unscaled + length_unscaled - 1) >> 12;
    u32 block_first = page_first >> 5;
    u32 block_last = page_last >> 5;
    auto watch_lock = GetSharedMemory().AcquireLock();
    for (u32 i = block_first; i <= block_last; ++i) {
      u32 add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      scaled_resolve_pages_[i] |= add_bits;
      scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
    }
  }

  // Toggling a texture between scaled and unscaled relies on this invalidation.
  GetSharedMemory().RangeWrittenByGPU(start_unscaled, length_unscaled);
}

u32 TextureCache::GuestToHostSwizzle(u32 guest_swizzle, u32 host_format_swizzle) {
  u32 host_swizzle = 0;
  for (u32 i = 0; i < 4; ++i) {
    u32 guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    u32 host_swizzle_component;
    if (guest_swizzle_component >= xenos::REXGPU_SWIZZLE_0) {
      // Swizzle values 6 and 7 are invalid on the host, so map them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component = (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

bool TextureCache::PrepareTextureLoad(Texture& texture, PendingTextureLoad& pending_load_out,
                                      PendingSharedMemoryRange* pending_ranges_out,
                                      size_t& pending_range_count_out) {
  u32 outdated_mask = texture.GetOutdatedMask();
  if (!outdated_mask) {
    return false;
  }

  bool base_outdated = false;
  bool mips_outdated = false;
  {
    auto watch_lock = GetSharedMemory().AcquireLock();
    if (outdated_mask & Texture::kOutdatedBitBase) {
      base_outdated = texture.IsBaseOutdated(watch_lock);
    }
    if (outdated_mask & Texture::kOutdatedBitMips) {
      mips_outdated = texture.IsMipsOutdated(watch_lock);
    }
  }
  if (!base_outdated && !mips_outdated) {
    return false;
  }

  pending_load_out.texture = &texture;
  pending_load_out.load_base = base_outdated;
  pending_load_out.load_mips = mips_outdated;
  pending_range_count_out = 0;

  TextureKey texture_key = texture.GetKey();
  if (base_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.base_page << 12;
    pending_range.length =
        static_cast<u32>(rex::RoundUpPow2(texture.GetGuestBaseSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }
  if (mips_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.mip_page << 12;
    pending_range.length =
        static_cast<u32>(rex::RoundUpPow2(texture.GetGuestMipsSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }

  return true;
}

bool TextureCache::CommitPreparedTextureLoad(const PendingTextureLoad& pending_load) {
  if (!pending_load.texture || (!pending_load.load_base && !pending_load.load_mips)) {
    return true;
  }

  Texture& texture = *pending_load.texture;
  TextureKey texture_key = texture.GetKey();
  if (texture_key.scaled_resolve) {
    // Padding not yet touched by a resolve counts in the texture size, so it must be resident.
    if (pending_load.load_base &&
        !CommitScaledResolveMemory(texture_key.base_page << 12, texture.GetGuestBaseSize(), 4)) {
      return false;
    }
    if (pending_load.load_mips &&
        !CommitScaledResolveMemory(texture_key.mip_page << 12, texture.GetGuestMipsSize(), 4)) {
      return false;
    }
  }

  if (!LoadTextureDataFromResidentMemoryImpl(texture, pending_load.load_base,
                                             pending_load.load_mips)) {
    return false;
  }

  // Scaled resolves are watched too, to detect CPU reuse of the memory.
  texture.UpdateAndWatch(GetSharedMemory().AcquireLock());
  texture.LogAction("Loaded");

  return true;
}

void TextureCache::RequestTextures(u32 used_texture_mask) {
  const auto& regs = GetRegisterFile();

  if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {
    ResetTextureBindings();
  }

  u32 bindings_changed = 0;
  std::vector<PendingTextureLoad> pending_texture_loads;
  std::vector<PendingSharedMemoryRange> pending_shared_memory_ranges;
  auto queue_pending_texture_load = [&](Texture* texture) {
    if (texture == nullptr) {
      return;
    }
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture == texture) {
        return;
      }
    }
    PendingTextureLoad pending_load;
    PendingSharedMemoryRange pending_ranges[2];
    size_t pending_range_count = 0;
    if (!PrepareTextureLoad(*texture, pending_load, pending_ranges, pending_range_count)) {
      return;
    }
    pending_texture_loads.push_back(pending_load);
    for (size_t i = 0; i < pending_range_count; ++i) {
      pending_shared_memory_ranges.push_back(pending_ranges[i]);
    }
  };
  u32 textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  u32 index = 0;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    u32 index_bit = UINT32_C(1) << index;
    textures_remaining &= ~index_bit;
    TextureBinding& binding = texture_bindings_[index];
    xenos::TextureFetch fetch = regs.GetTextureFetch(index);
    TextureKey old_key = binding.key;
    u8 old_swizzled_signs = binding.swizzled_signs;
    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
    texture_bindings_in_sync_ |= index_bit;
    if (!binding.key.is_valid) {
      if (old_key.is_valid) {
        bindings_changed |= index_bit;
      }
      binding.Reset();
      continue;
    }
    u32 old_host_swizzle = binding.host_swizzle;
    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));

    bool key_changed = binding.key != old_key;
    bool any_sign_was_not_signed = xenos::HasNonSignedComponent(old_swizzled_signs);
    bool any_sign_was_signed = xenos::HasSignedComponent(old_swizzled_signs);
    bool any_sign_is_not_signed = xenos::HasNonSignedComponent(binding.swizzled_signs);
    bool any_sign_is_signed = xenos::HasSignedComponent(binding.swizzled_signs);
    if (key_changed || binding.host_swizzle != old_host_swizzle ||
        any_sign_is_not_signed != any_sign_was_not_signed ||
        any_sign_is_signed != any_sign_was_signed) {
      bindings_changed |= index_bit;
    }
    bool load_unsigned_data = false, load_signed_data = false;
    if (IsSignedVersionSeparateForFormat(binding.key)) {
      // Previous signedness, not binding.texture, is checked so failed loads are not retried.
      if (any_sign_is_not_signed) {
        if (key_changed || !any_sign_was_not_signed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        }
      } else {
        binding.texture = nullptr;
      }
      if (any_sign_is_signed) {
        if (key_changed || !any_sign_was_signed) {
          TextureKey signed_key = binding.key;
          signed_key.signed_separate = 1;
          binding.texture_signed = FindOrCreateTexture(signed_key);
          load_signed_data = true;
        }
      } else {
        binding.texture_signed = nullptr;
      }
    } else {
      if (key_changed) {
        binding.texture = FindOrCreateTexture(binding.key);
        load_unsigned_data = true;
      }
      binding.texture_signed = nullptr;
    }
    if (load_unsigned_data) {
      queue_pending_texture_load(binding.texture);
    }
    if (load_signed_data) {
      queue_pending_texture_load(binding.texture_signed);
    }
  }

  REX_PROFILE_PLOT("gpu/texture_cache/request_textures_pending_load_count",
                   static_cast<u32>(pending_texture_loads.size()));
  REX_PROFILE_PLOT("gpu/texture_cache/request_textures_pending_range_count",
                   static_cast<u32>(pending_shared_memory_ranges.size()));

  bool batched_shared_memory_request_succeeded = true;
  std::vector<std::pair<u32, u32>> pending_shared_memory_range_pairs;
  if (!pending_shared_memory_ranges.empty()) {
    pending_shared_memory_range_pairs.reserve(pending_shared_memory_ranges.size());
    for (const PendingSharedMemoryRange& pending_range : pending_shared_memory_ranges) {
      pending_shared_memory_range_pairs.emplace_back(pending_range.start, pending_range.length);
    }
    batched_shared_memory_request_succeeded = GetSharedMemory().RequestRanges(
        pending_shared_memory_range_pairs.data(), pending_shared_memory_range_pairs.size());
  }

  if (batched_shared_memory_request_succeeded) {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      CommitPreparedTextureLoad(pending_load);
    }
  } else {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
        LoadTextureData(*pending_load.texture);
      }
    }
  }
  if (bindings_changed) {
    UpdateTextureBindingsImpl(bindings_changed);
  }
}

const char* TextureCache::TextureKey::GetLogDimensionName(xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      REX_UNHANDLED_CASE(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  REXLOG_TRACE(gpu,
               "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
               "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
               action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "", GetWidth(),
               GetHeight(), GetDepthOrArraySize(), GetLogDimensionName(),
               xenos::FormatInfo::Get(format)->name, mip_max_level + 1, packed_mips ? "" : "un",
               mip_max_level != 0 ? "s" : "", base_page << 12, pitch << 5, mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  REXLOG_TRACE(gpu,
               "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
               "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
               "0x{:08X})",
               action, key_.tiled ? "tiled" : "linear", key_.scaled_resolve ? "scaled " : "",
               key_.GetWidth(), key_.GetHeight(), key_.GetDepthOrArraySize(),
               key_.GetLogDimensionName(), xenos::FormatInfo::Get(key_.format)->name,
               key_.mip_max_level + 1, key_.packed_mips ? "" : "un",
               key_.mip_max_level != 0 ? "s" : "", key_.base_page << 12, key_.pitch << 5,
               GetGuestBaseSize(), key_.mip_page << 12, GetGuestMipsSize());
}

TextureCache::Texture::Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  base_outdated_ = GetGuestLayout().base.level_data_extent_bytes != 0;
  mips_outdated_ = GetGuestLayout().mips_total_extent_bytes != 0;
  outdated_mask_.store(
      (base_outdated_ ? kOutdatedBitBase : 0) | (mips_outdated_ ? kOutdatedBitMips : 0),
      std::memory_order_relaxed);
}

TextureCache::Texture::~Texture() {
  if (mips_watch_handle_) {
    GetTextureCache().GetSharedMemory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    GetTextureCache().GetSharedMemory().UnwatchMemoryRange(base_watch_handle_);
  }

  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

void TextureCache::Texture::UpdateAndWatch(
    const std::unique_lock<rex::platform::RankedMutex>& watch_lock) {
  SharedMemory& shared_memory = GetTextureCache().GetSharedMemory();
  if (base_outdated_) {
    REX_ASSERT(GetGuestBaseSize() != 0);
    base_outdated_ = false;
    base_watch_handle_ =
        shared_memory.WatchMemoryRange(watch_lock, GetKey().base_page << 12, GetGuestBaseSize(),
                                       TextureCache::WatchCallback, this, nullptr, 0);
    outdated_mask_.fetch_and(~kOutdatedBitBase, std::memory_order_release);
  }
  if (mips_outdated_) {
    REX_ASSERT(GetGuestMipsSize() != 0);
    mips_outdated_ = false;
    mips_watch_handle_ =
        shared_memory.WatchMemoryRange(watch_lock, GetKey().mip_page << 12, GetGuestMipsSize(),
                                       TextureCache::WatchCallback, this, nullptr, 1);
    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);
  }
}

void TextureCache::Texture::MarkAsUsed() {
  if (!in_usage_list_) {
    return;
  }
  REX_ASSERT(last_usage_submission_index_ <= texture_cache_.current_submission_index_);
  if (last_usage_submission_index_ >= texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

void TextureCache::Texture::WatchCallback(bool is_mip) {
  if (is_mip) {
    REX_ASSERT(GetGuestMipsSize() != 0);
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitMips, std::memory_order_release);
  } else {
    REX_ASSERT(GetGuestBaseSize() != 0);
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitBase, std::memory_order_release);
  }
}

void TextureCache::WatchCallback(void* context, [[maybe_unused]] void* data, u64 argument,
                                 [[maybe_unused]] bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  texture.WatchCallback(argument != 0);
  texture.GetTextureCache().texture_became_outdated_.store(true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  REX_PROFILE_PLOT("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  if (IsDrawResolutionScaled() && key.tiled && IsScaledResolveSupportedForFormat(key)) {
    xenos::GuestTextureLayout scaled_resolve_guest_layout = key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(key.base_page << 12,
                               scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(key.mip_page << 12,
                               scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  auto get_host_extent = [this, &key](u32& host_width_out, u32& host_height_out,
                                      u32& host_depth_or_array_size_out) {
    host_width_out = key.GetWidth();
    host_height_out = key.GetHeight();
    if (key.scaled_resolve) {
      host_width_out *= GetDrawResolutionScaleX();
      host_height_out *= GetDrawResolutionScaleY();
    }
    host_depth_or_array_size_out = key.GetDepthOrArraySize();
  };
  auto try_drop_top_level = [&key]() {
    if (!key.mip_page || !key.mip_max_level) {
      return false;
    }
    u32 old_width = key.GetWidth();
    u32 old_height = key.GetHeight();
    u32 old_depth_or_array_size = key.GetDepthOrArraySize();
    u32 new_width = std::max(old_width >> 1, UINT32_C(1));
    u32 new_height = std::max(old_height >> 1, UINT32_C(1));
    u32 new_depth_or_array_size = key.dimension == xenos::DataDimension::k3D
                                      ? std::max(old_depth_or_array_size >> 1, UINT32_C(1))
                                      : old_depth_or_array_size;
    if (new_width == old_width && new_height == old_height &&
        new_depth_or_array_size == old_depth_or_array_size) {
      return false;
    }
    const xenos::FormatInfo* format_info = xenos::FormatInfo::Get(key.format);
    u32 rebased_pitch_texels = std::max(rex::next_pow2(old_width) >> 1, UINT32_C(1));
    rebased_pitch_texels = rex::RoundUpPow2(rebased_pitch_texels, format_info->block_width);
    rebased_pitch_texels = rex::RoundUpPow2(rebased_pitch_texels, xenos::kTextureTileWidthHeight);
    if (!key.tiled) {
      u32 rebased_pitch_blocks = rebased_pitch_texels / format_info->block_width;
      u32 rebased_row_pitch_bytes = rebased_pitch_blocks * format_info->GetBytesPerBlock();
      rebased_row_pitch_bytes =
          rex::RoundUpPow2(rebased_row_pitch_bytes, xenos::kTextureLinearRowAlignmentBytes);
      rebased_pitch_blocks = rebased_row_pitch_bytes / format_info->GetBytesPerBlock();
      rebased_pitch_texels = rebased_pitch_blocks * format_info->block_width;
    }
    u32 rebased_pitch = std::max(rebased_pitch_texels >> 5, UINT32_C(1));
    if (rebased_pitch > ((UINT32_C(1) << 9) - 1)) {
      return false;
    }
    key.width_minus_1 = new_width - 1;
    key.height_minus_1 = new_height - 1;
    key.depth_or_array_size_minus_1 = new_depth_or_array_size - 1;
    key.base_page = key.mip_page;
    key.mip_page = 0;
    key.pitch = rebased_pitch;
    key.mip_max_level = 0;
    key.packed_mips = 0;
    return true;
  };
  u32 host_width, host_height, host_depth_or_array_size;
  get_host_extent(host_width, host_height, host_depth_or_array_size);
  u32 max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  u32 max_host_depth_or_array_size = GetMaxHostTextureDepthOrArraySize(key.dimension);
  while (true) {
    bool width_height_too_large =
        host_width > max_host_width_height || host_height > max_host_width_height;
    bool depth_or_array_too_large = host_depth_or_array_size > max_host_depth_or_array_size;
    if (!width_height_too_large && !depth_or_array_too_large) {
      break;
    }
    bool size_adjusted = false;
    if (key.scaled_resolve) {
      key.scaled_resolve = 0;
      size_adjusted = true;
    } else if ((width_height_too_large ||
                (depth_or_array_too_large && key.dimension == xenos::DataDimension::k3D)) &&
               try_drop_top_level()) {
      size_adjusted = true;
    }
    if (!size_adjusted) {
      return nullptr;
    }
    get_host_extent(host_width, host_height, host_depth_or_array_size);
  }

  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      return nullptr;
    }
    REX_ASSERT(new_texture->GetKey() == key);
    texture = textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  REX_PROFILE_PLOT("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  return texture;
}

bool TextureCache::LoadTextureData(Texture& texture) {
  PendingTextureLoad pending_load;
  PendingSharedMemoryRange pending_ranges[2];
  size_t pending_range_count = 0;
  if (!PrepareTextureLoad(texture, pending_load, pending_ranges, pending_range_count)) {
    return true;
  }

  std::pair<u32, u32> pending_range_pairs[2];
  for (size_t i = 0; i < pending_range_count; ++i) {
    pending_range_pairs[i] = std::make_pair(pending_ranges[i].start, pending_ranges[i].length);
  }
  if (!GetSharedMemory().RequestRanges(pending_range_pairs, pending_range_count)) {
    return false;
  }

  return CommitPreparedTextureLoad(pending_load);
}

void TextureCache::BindingInfoFromFetchConstant(const xenos::TextureFetch& fetch,
                                                TextureKey& key_out, u8* swizzled_signs_out) {
  key_out.Invalidate();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out =
        static_cast<u8>(xenos::TextureSign::kUnsigned) * static_cast<u8>(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (REXCVAR_GET(::rex::gpu::cvars::allow_invalid_fetch_constants)) {
        break;
      }
      REXLOG_WARN(gpu,
                  "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
                  "has \"invalid\" type! This is incorrect behavior, but you can try "
                  "bypassing this by setting "
                  "xenos.allow_invalid_fetch_constants=true.",
                  fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                  fetch.dword_5);
      return;
    default:
      REXLOG_WARN(gpu,
                  "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
                  "is invalid!",
                  fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                  fetch.dword_5);
      return;
  }

  u32 width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  u32 base_page, mip_page, mip_max_level;
  xenos::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                          &depth_or_array_size_minus_1, &base_page, &mip_page,
                                          nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    return;
  }
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      REXLOG_ERROR(gpu,
                   "1D texture is too wide ({}) and is ignored. Report the title to ReXGlue "
                   "developers",
                   width_minus_1 + 1);
      is_invalid_1d = true;
    }
    REX_ASSERT(!fetch.tiled);
    if (fetch.tiled) {
      REXLOG_ERROR(gpu,
                   "1D texture has tiling enabled in the fetch constant, but this "
                   "appears to be wrong and is ignored. Report the title to ReXGlue "
                   "developers");
      is_invalid_1d = true;
    }
    REX_ASSERT(!fetch.packed_mips);
    if (fetch.packed_mips) {
      REXLOG_ERROR(gpu,
                   "1D texture has packed mips enabled in the fetch constant, but this "
                   "appears to be wrong and is ignored. Report the title to ReXGlue "
                   "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = fetch.pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = xenos::SwizzleSigns(fetch);
  }
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  u32 bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(u64 add, u64 subtract) {
  textures_total_host_memory_usage_ = textures_total_host_memory_usage_ - subtract + add;
  REX_PROFILE_PLOT(
      "gpu/texture_cache/total_host_memory_usage_mb",
      static_cast<u32>((textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20));
}

bool TextureCache::IsRangeScaledResolved(u32 start_unscaled, u32 length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  u32 page_first = start_unscaled >> 12;
  u32 page_last = (start_unscaled + length_unscaled - 1) >> 12;
  u32 block_first = page_first >> 5;
  u32 block_last = page_last >> 5;
  u32 l2_block_first = block_first >> 6;
  u32 l2_block_last = block_last >> 6;
  auto watch_lock = GetSharedMemory().AcquireLock();
  for (u32 i = l2_block_first; i <= l2_block_last; ++i) {
    u64 l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    u32 block_relative_index;
    while (rex::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      u32 block_index = (i << 6) + block_relative_index;
      u32 check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(void* context, u32 address_first,
                                                         u32 address_last,
                                                         bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(address_first, address_last, invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(u32 address_first, u32 address_last,
                                                    bool invalidated_by_gpu) {
  REX_ASSERT(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // Resolves set these bits themselves, so invalidations by the GPU are ignored.
    return;
  }
  u32 resolve_page_first = address_first >> 12;
  u32 resolve_page_last = address_last >> 12;
  u32 resolve_block_first = resolve_page_first >> 5;
  u32 resolve_block_last = resolve_page_last >> 5;
  u32 resolve_l2_block_first = resolve_block_first >> 6;
  u32 resolve_l2_block_last = resolve_block_last >> 6;
  for (u32 i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    u64 resolve_l2_block = scaled_resolve_pages_l2_[i];
    if (REXCVAR_GET(::rex::gpu::cvars::pre_mask_resolve_l2_block)) {
      if (i == resolve_l2_block_first) {
        resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
      }
      if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
        resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
      }
    }
    u32 resolve_block_relative_index;
    while (rex::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      u32 resolve_block_index = (i << 6) + resolve_block_relative_index;
      u32 resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last && (resolve_page_last & 31) != 31) {
        resolve_keep_bits |= ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &= ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
}

}  // namespace rex::gpu
