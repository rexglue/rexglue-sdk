/**
 * @file        base/host_alloc.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/memory/host.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <new>

#include <rex/platform/process.h>
#include <rex/platform/perf.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <malloc.h>
#endif

namespace rex::memory {

namespace {

#ifdef NDEBUG
constexpr bool kChecks = false;
#else
constexpr bool kChecks = true;
#endif

constexpr size_t kHeaderSize = 16;
constexpr size_t kTagCount = kMaxTags;
constexpr u32 kLiveMagic = 0xA110C8ED;
constexpr u32 kFreedMagic = 0xDEADF00D;
constexpr u8 kNewFill = 0xCD;
constexpr u8 kFreedFill = 0xDD;
constexpr size_t kSizeBits = 48;
constexpr u64 kSizeMask = (u64{1} << kSizeBits) - 1;
[[maybe_unused]] constexpr u64 kReportedBit = u64{1} << 63;

struct Header {
  u32 magic;
  u32 base_offset;
  u64 packed;
};
static_assert(sizeof(Header) == kHeaderSize);

constexpr size_t kShardCount = 32;
constexpr u32 kNoShard = ~u32{0};
constexpr u64 kPeakSampleMask = 4095;

struct alignas(32) Counters {
  std::atomic<u64> alloc_bytes{0};
  std::atomic<u64> alloc_count{0};
  std::atomic<u64> free_bytes{0};
  std::atomic<u64> free_count{0};
};

struct alignas(64) Peak {
  std::atomic<u64> bytes{0};
};

struct ThreadState {
  Tag tag;
  u32 shard;
};

struct alignas(64) ShardRow {
  Counters tags[kTagCount];
};
static_assert(sizeof(ShardRow) % 64 == 0);

constinit ShardRow g_shards[kShardCount];
constinit Peak g_peaks[kTagCount];
constinit Peak g_peak_total;
constinit std::atomic<u32> g_next_shard{0};

constexpr i32 kMaxStackDepth = 32;
constinit std::atomic<i32> g_stack_depth{0};
constinit std::atomic<u32> g_fail_count{0};
constinit std::atomic<detail::BlockErrorHandler> g_error_handler{nullptr};
constinit thread_local ThreadState t_thread = {Tag::kCore, kNoShard};
#ifdef REXGLUE_ENABLE_PROFILING
constinit thread_local bool t_reporting = false;
#endif

constexpr size_t kTagNameCapacity = 32;

struct TagNameText {
  char slot[2][kTagNameCapacity];
  u32 next;
};

constinit TagNameText g_tag_name_text[kTagCount]{};
constinit std::atomic<const char*> g_tag_names[kTagCount] = {{"Core"}, {"ThirdParty"}};

#ifdef REXGLUE_ENABLE_PROFILING
const char* GetPoolName(Tag tag) noexcept {
  const char* name = g_tag_names[static_cast<size_t>(tag)].load(std::memory_order_relaxed);
  return name ? name : "Unnamed";
}
#endif

void RaisePeak(Peak& peak, u64 live) noexcept {
  u64 seen = peak.bytes.load(std::memory_order_relaxed);
  while (live > seen && !peak.bytes.compare_exchange_weak(seen, live, std::memory_order_relaxed)) {}
}

u32 ShardOfThread() noexcept {
  u32 shard = t_thread.shard;
  if (shard == kNoShard) {
    shard = g_next_shard.fetch_add(1, std::memory_order_relaxed) % kShardCount;
    t_thread.shard = shard;
  }
  return shard;
}

struct TagTotals {
  u64 live_bytes;
  u64 live_count;
  u64 total_count;
};

TagTotals SumTag(size_t tag) noexcept {
  u64 freed_bytes = 0;
  u64 freed_count = 0;
  for (size_t shard = 0; shard < kShardCount; ++shard) {
    const Counters& counters = g_shards[shard].tags[tag];
    freed_bytes += counters.free_bytes.load(std::memory_order_relaxed);
    freed_count += counters.free_count.load(std::memory_order_relaxed);
  }
  u64 bytes = 0;
  u64 count = 0;
  for (size_t shard = 0; shard < kShardCount; ++shard) {
    const Counters& counters = g_shards[shard].tags[tag];
    bytes += counters.alloc_bytes.load(std::memory_order_relaxed);
    count += counters.alloc_count.load(std::memory_order_relaxed);
  }
  return {bytes > freed_bytes ? bytes - freed_bytes : 0,
          count > freed_count ? count - freed_count : 0, count};
}

void SamplePeaks(size_t tag) noexcept {
  RaisePeak(g_peaks[tag], SumTag(tag).live_bytes);
  u64 all = 0;
  for (size_t i = 0; i < kTagCount; ++i) {
    all += SumTag(i).live_bytes;
  }
  RaisePeak(g_peak_total, all);
}

void CountAlloc(Tag tag, u64 size) noexcept {
  const size_t index = static_cast<size_t>(tag);
  Counters& counters = g_shards[ShardOfThread()].tags[index];
  counters.alloc_bytes.fetch_add(size, std::memory_order_relaxed);
  const u64 count = counters.alloc_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((count & kPeakSampleMask) == 0) {
    SamplePeaks(index);
  }
}

void CountFree(Tag tag, u64 size) noexcept {
  Counters& counters = g_shards[ShardOfThread()].tags[static_cast<size_t>(tag)];
  counters.free_bytes.fetch_add(size, std::memory_order_relaxed);
  counters.free_count.fetch_add(1, std::memory_order_relaxed);
}

#ifdef REXGLUE_ENABLE_PROFILING

bool ReportAlloc(const void* ptr, size_t size, Tag tag) noexcept {
  if (t_reporting || !TracyIsStarted) {
    return false;
  }
  t_reporting = true;
  const char* pool = GetPoolName(tag);
  const i32 depth = g_stack_depth.load(std::memory_order_relaxed);
  if (depth > 0) {
    REX_PROFILE_ALLOC_NAMED_STACK(ptr, size, depth, pool);
  } else {
    REX_PROFILE_ALLOC_NAMED(ptr, size, pool);
  }
  t_reporting = false;
  return true;
}

void ReportFree(const void* ptr, Tag tag) noexcept {
  if (t_reporting) {
    return;
  }
  t_reporting = true;
  const char* pool = GetPoolName(tag);
  const i32 depth = g_stack_depth.load(std::memory_order_relaxed);
  if (depth > 0) {
    REX_PROFILE_FREE_NAMED_STACK(ptr, depth, pool);
  } else {
    REX_PROFILE_FREE_NAMED(ptr, pool);
  }
  t_reporting = false;
}

#endif

void* BackendAlloc(size_t bytes, size_t align) noexcept {
#if REX_PLATFORM_WIN32
  return align > kHeaderSize ? _aligned_malloc(bytes, align) : std::malloc(bytes);
#else
  if (align > kHeaderSize) {
    void* block = nullptr;
    return posix_memalign(&block, align, bytes) == 0 ? block : nullptr;
  }
  return std::malloc(bytes);
#endif
}

void BackendFree(void* base, bool aligned) noexcept {
#if REX_PLATFORM_WIN32
  if (aligned) {
    _aligned_free(base);
    return;
  }
#else
  (void)aligned;
#endif
  std::free(base);
}

u64 Pack(u64 size, Tag tag, detail::AllocType type) noexcept {
  return size | (static_cast<u64>(tag) << kSizeBits) | (static_cast<u64>(type) << 56);
}

Header* HeaderOf(void* ptr) noexcept {
  return reinterpret_cast<Header*>(static_cast<u8*>(ptr) - kHeaderSize);
}

[[maybe_unused]] void ReportError(detail::BlockError error, const void* ptr) noexcept {
  if (auto handler = g_error_handler.load(std::memory_order_relaxed)) {
    handler(error, ptr);
    return;
  }
  static constexpr const char* kText[] = {
      "pointer was not made by the host allocator or was freed already",
      "block was freed with the other form of new or delete",
      "size given to delete differs from the size requested",
  };
  std::fprintf(stderr, "host allocator: %p: %s\n", ptr, kText[static_cast<size_t>(error)]);
  rex::platform::Break();
  std::abort();
}

void* AllocBlock(size_t size, size_t align, detail::AllocType type) noexcept {
  if (g_fail_count.load(std::memory_order_relaxed) != 0 &&
      g_fail_count.fetch_sub(1, std::memory_order_relaxed) != 0) {
    return nullptr;
  }
  if (align != 0 && !std::has_single_bit(align)) {
    return nullptr;
  }
  const size_t effective = align > kHeaderSize ? align : kHeaderSize;
  if (size > kSizeMask || size > SIZE_MAX - effective - kHeaderSize) {
    return nullptr;
  }
  const size_t offset = effective;
  void* base = BackendAlloc(offset + size, effective);
  if (!base) {
    return nullptr;
  }
  u8* user = static_cast<u8*>(base) + offset;
  const Tag tag = t_thread.tag;
  Header* header = HeaderOf(user);
  header->magic = kLiveMagic;
  header->base_offset = static_cast<u32>(offset);
  header->packed = Pack(size, tag, type);
  if constexpr (kChecks) {
    std::memset(user, kNewFill, size);
  }
#ifdef REXGLUE_ENABLE_PROFILING
  if (ReportAlloc(user, size, tag)) {
    header->packed |= kReportedBit;
  }
#endif
  CountAlloc(tag, size);
  return user;
}

}  // namespace

void* HostAlloc(size_t size, size_t align) noexcept {
  return AllocBlock(size, align, detail::AllocType::kRaw);
}

void HostFree(void* ptr, size_t size, size_t align) noexcept {
  (void)align;
  detail::DeleteBlock(ptr, size, detail::AllocType::kRaw);
}

size_t HostSize(const void* ptr) noexcept {
  const Header* header = reinterpret_cast<const Header*>(static_cast<const u8*>(ptr) - kHeaderSize);
  return static_cast<size_t>(header->packed & kSizeMask);
}

HostStats GetHostStats() noexcept {
  HostStats stats;
  u64 all_bytes = 0;
  for (size_t i = 0; i < kTagCount; ++i) {
    const TagTotals totals = SumTag(i);
    RaisePeak(g_peaks[i], totals.live_bytes);
    TagStats& to = stats.tags[i];
    to.live_bytes = totals.live_bytes;
    to.live_count = totals.live_count;
    to.peak_bytes = g_peaks[i].bytes.load(std::memory_order_relaxed);
    to.total_count = totals.total_count;
    stats.total.live_bytes += totals.live_bytes;
    stats.total.live_count += totals.live_count;
    stats.total.total_count += totals.total_count;
    all_bytes += totals.live_bytes;
  }
  RaisePeak(g_peak_total, all_bytes);
  stats.total.peak_bytes = g_peak_total.bytes.load(std::memory_order_relaxed);
  return stats;
}

void SetAllocStackDepth(i32 depth) noexcept {
  g_stack_depth.store(depth < 0 ? 0 : (depth > kMaxStackDepth ? kMaxStackDepth : depth),
                      std::memory_order_relaxed);
}

i32 GetAllocStackDepth() noexcept {
  return g_stack_depth.load(std::memory_order_relaxed);
}

Tag GetThreadTag() noexcept {
  return t_thread.tag;
}

const char* GetTagName(Tag tag) noexcept {
  const size_t index = static_cast<size_t>(tag);
  const char* name =
      index < kTagCount ? g_tag_names[index].load(std::memory_order_relaxed) : nullptr;
  return name ? name : "Invalid";
}

void SetTagName(Tag tag, const char* name) noexcept {
  const size_t index = static_cast<size_t>(tag);
  if (index < kTagCount && name != nullptr) {
    TagNameText& text = g_tag_name_text[index];
    char* copy = text.slot[text.next++ & 1];
    const size_t length = std::min(std::strlen(name), kTagNameCapacity - 1);
    std::memcpy(copy, name, length);
    copy[length] = '\0';
    g_tag_names[index].store(copy, std::memory_order_release);
  }
}

TagScope::TagScope(Tag tag) noexcept : previous_(t_thread.tag) {
  t_thread.tag = static_cast<size_t>(tag) < kTagCount ? tag : Tag::kCore;
}

TagScope::~TagScope() {
  t_thread.tag = previous_;
}

namespace detail {

BlockErrorHandler SetBlockErrorHandler(BlockErrorHandler handler) noexcept {
  return g_error_handler.exchange(handler, std::memory_order_relaxed);
}

void FailNextAllocations(u32 count) noexcept {
  g_fail_count.store(count, std::memory_order_relaxed);
}

void* NewBlock(size_t size, size_t align, AllocType type, bool can_throw) {
  for (;;) {
    if (void* block = AllocBlock(size, align, type)) {
      return block;
    }
    const std::new_handler handler = std::get_new_handler();
    if (!handler) {
      if (!can_throw) {
        return nullptr;
      }
      std::fprintf(stderr, "host allocator: out of memory, %zu bytes requested for tag %s\n", size,
                   GetTagName(t_thread.tag));
      throw std::bad_alloc();
    }
    if (can_throw) {
      handler();
    } else {
      try {
        handler();
      } catch (...) {
        return nullptr;
      }
    }
  }
}

void DeleteBlock(void* ptr, size_t size, AllocType type) noexcept {
  if (!ptr) {
    return;
  }
  Header* header = HeaderOf(ptr);
  if constexpr (kChecks) {
    if (reinterpret_cast<uintptr_t>(ptr) % kHeaderSize != 0 || header->magic != kLiveMagic) {
      ReportError(BlockError::kForeignPointer, ptr);
      return;
    }
    const AllocType made = static_cast<AllocType>((header->packed >> 56) & 0x7F);
    if (made != type && made != AllocType::kRaw && type != AllocType::kRaw) {
      ReportError(BlockError::kTypeMismatch, ptr);
      return;
    }
    if (size != 0 && size != (header->packed & kSizeMask)) {
      ReportError(BlockError::kSizeMismatch, ptr);
      return;
    }
  }
  const size_t block_size = static_cast<size_t>(header->packed & kSizeMask);
  const Tag tag = static_cast<Tag>((header->packed >> kSizeBits) & 0xFF);
  const size_t offset = header->base_offset;
#ifdef REXGLUE_ENABLE_PROFILING
  if (header->packed & kReportedBit) {
    ReportFree(ptr, tag);
  }
#endif
  if constexpr (kChecks) {
    header->magic = kFreedMagic;
    std::memset(ptr, kFreedFill, block_size);
  }
  CountFree(tag, block_size);
  BackendFree(static_cast<u8*>(ptr) - offset, offset > kHeaderSize);
}

}  // namespace detail

}  // namespace rex::memory
