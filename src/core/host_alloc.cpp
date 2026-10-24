/**
 * @file        core/host_alloc.cpp
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
#include <cstring>
#include <new>

#include <rex/dbg.h>
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
constexpr size_t kTagCount = static_cast<size_t>(Tag::kCount);
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

struct alignas(64) Counters {
  std::atomic<u64> live_bytes{0};
  std::atomic<u64> live_count{0};
  std::atomic<u64> peak_bytes{0};
  std::atomic<u64> total_count{0};
};

constinit Counters g_tags[kTagCount];
constinit Counters g_total;

constexpr i32 kMaxStackDepth = 32;
constinit std::atomic<i32> g_stack_depth{0};
constinit std::atomic<u32> g_fail_count{0};
constinit std::atomic<detail::BlockErrorHandler> g_error_handler{nullptr};
constinit thread_local Tag t_tag = Tag::kCore;
#ifdef REXGLUE_ENABLE_PROFILING
constinit thread_local bool t_reporting = false;
#endif

constexpr const char* kTagNames[kTagCount] = {
    "Core", "Runtime", "GPU", "APU", "UI", "HID", "Filesystem", "Codegen", "ThirdParty",
};

void RaisePeak(Counters& counters, u64 live) noexcept {
  u64 peak = counters.peak_bytes.load(std::memory_order_relaxed);
  while (live > peak &&
         !counters.peak_bytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
}

void CountAlloc(Tag tag, u64 size) noexcept {
  Counters& counters = g_tags[static_cast<size_t>(tag)];
  const u64 live = counters.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
  counters.live_count.fetch_add(1, std::memory_order_relaxed);
  counters.total_count.fetch_add(1, std::memory_order_relaxed);
  RaisePeak(counters, live);
  const u64 all = g_total.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
  g_total.live_count.fetch_add(1, std::memory_order_relaxed);
  g_total.total_count.fetch_add(1, std::memory_order_relaxed);
  RaisePeak(g_total, all);
}

void CountFree(Tag tag, u64 size) noexcept {
  Counters& counters = g_tags[static_cast<size_t>(tag)];
  counters.live_bytes.fetch_sub(size, std::memory_order_relaxed);
  counters.live_count.fetch_sub(1, std::memory_order_relaxed);
  g_total.live_bytes.fetch_sub(size, std::memory_order_relaxed);
  g_total.live_count.fetch_sub(1, std::memory_order_relaxed);
}

#ifdef REXGLUE_ENABLE_PROFILING

bool ReportAlloc(const void* ptr, size_t size, Tag tag) noexcept {
  if (t_reporting || !TracyIsStarted) {
    return false;
  }
  t_reporting = true;
  const char* pool = kTagNames[static_cast<size_t>(tag)];
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
  const char* pool = kTagNames[static_cast<size_t>(tag)];
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

u64 Pack(u64 size, Tag tag, detail::Kind kind) noexcept {
  return size | (static_cast<u64>(tag) << kSizeBits) | (static_cast<u64>(kind) << 56);
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

void* AllocBlock(size_t size, size_t align, detail::Kind kind) noexcept {
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
  const Tag tag = t_tag;
  Header* header = HeaderOf(user);
  header->magic = kLiveMagic;
  header->base_offset = static_cast<u32>(offset);
  header->packed = Pack(size, tag, kind);
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
  return AllocBlock(size, align, detail::Kind::kRaw);
}

void HostFree(void* ptr, size_t size, size_t align) noexcept {
  (void)align;
  detail::DeleteBlock(ptr, size, detail::Kind::kRaw);
}

size_t HostSize(const void* ptr) noexcept {
  const Header* header = reinterpret_cast<const Header*>(static_cast<const u8*>(ptr) - kHeaderSize);
  return static_cast<size_t>(header->packed & kSizeMask);
}

HostStats GetHostStats() noexcept {
  HostStats stats;
  const auto read = [](const Counters& from, TagStats& to) {
    to.live_bytes = from.live_bytes.load(std::memory_order_relaxed);
    to.live_count = from.live_count.load(std::memory_order_relaxed);
    to.peak_bytes = from.peak_bytes.load(std::memory_order_relaxed);
    to.total_count = from.total_count.load(std::memory_order_relaxed);
  };
  for (size_t i = 0; i < kTagCount; ++i) {
    read(g_tags[i], stats.tags[i]);
  }
  read(g_total, stats.total);
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
  return t_tag;
}

const char* GetTagName(Tag tag) noexcept {
  const size_t index = static_cast<size_t>(tag);
  return index < kTagCount ? kTagNames[index] : "Invalid";
}

TagScope::TagScope(Tag tag) noexcept : previous_(t_tag) {
  t_tag = tag;
}

TagScope::~TagScope() {
  t_tag = previous_;
}

namespace detail {

BlockErrorHandler SetBlockErrorHandler(BlockErrorHandler handler) noexcept {
  return g_error_handler.exchange(handler, std::memory_order_relaxed);
}

void FailNextAllocations(u32 count) noexcept {
  g_fail_count.store(count, std::memory_order_relaxed);
}

void* NewBlock(size_t size, size_t align, Kind kind, bool can_throw) {
  for (;;) {
    if (void* block = AllocBlock(size, align, kind)) {
      return block;
    }
    const std::new_handler handler = std::get_new_handler();
    if (!handler) {
      if (!can_throw) {
        return nullptr;
      }
      std::fprintf(stderr, "host allocator: out of memory, %zu bytes requested for tag %s\n", size,
                   GetTagName(t_tag));
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

void DeleteBlock(void* ptr, size_t size, Kind kind) noexcept {
  if (!ptr) {
    return;
  }
  Header* header = HeaderOf(ptr);
  if constexpr (kChecks) {
    if (reinterpret_cast<uintptr_t>(ptr) % kHeaderSize != 0 || header->magic != kLiveMagic) {
      ReportError(BlockError::kForeignPointer, ptr);
      return;
    }
    const Kind made = static_cast<Kind>((header->packed >> 56) & 0x7F);
    if (made != kind && made != Kind::kRaw && kind != Kind::kRaw) {
      ReportError(BlockError::kKindMismatch, ptr);
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
