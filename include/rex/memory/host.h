/**
 * @file        memory/host.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstddef>

#include <rex/export.h>
#include <rex/types.h>

namespace rex::memory {

/** Subsystem that owns a host allocation. */
enum class Tag : u8 {
  kCore,       /**< Default tag, also used for anything not covered by another tag. */
  kRuntime,    /**< Kernel objects and guest memory bookkeeping. */
  kGPU,        /**< Graphics emulation and the graphics backends. */
  kAPU,        /**< Audio emulation and the audio backends. */
  kUI,         /**< Windowing, input overlays and the debug UI. */
  kHID,        /**< Input devices. */
  kFilesystem, /**< Mounts, containers and host file access. */
  kCodegen,    /**< The recompiler. */
  kThirdParty, /**< Memory requested by libraries that have an allocation hook. */
  kCount,      /**< Number of tags, not a tag. */
};

/** Usage counters of one tag. */
struct TagStats {
  u64 live_bytes = 0;  /**< Bytes allocated and not yet freed. */
  u64 live_count = 0;  /**< Allocations not yet freed. */
  u64 peak_bytes = 0;  /**< Highest value live_bytes reached. */
  u64 total_count = 0; /**< Allocations made since the start of the process. */
};

/** Usage counters of every tag. */
struct HostStats {
  std::array<TagStats, static_cast<size_t>(Tag::kCount)> tags{}; /**< Counters indexed by Tag. */
  TagStats total;                                                /**< Counters over all tags. */
};

/**
 * Allocates host memory. The block belongs to the tag of the calling thread. Works at any time,
 * including before main and after Shutdown.
 *
 * @param size The size in bytes, 0 returns a unique block.
 * @param align The alignment in bytes, a power of 2, 0 for the default alignment of 16.
 * @return The block, or null when the system is out of memory or the alignment is not valid.
 */
REX_API void* HostAlloc(size_t size, size_t align = 0) noexcept;

/**
 * Frees a block made by HostAlloc.
 *
 * @param ptr The block, null does nothing.
 * @param size The size passed to HostAlloc, 0 when unknown.
 * @param align The alignment passed to HostAlloc, 0 when unknown.
 */
REX_API void HostFree(void* ptr, size_t size = 0, size_t align = 0) noexcept;

/**
 * Returns the requested size of a block made by HostAlloc.
 *
 * @param ptr The block, not null.
 * @return The size in bytes.
 */
REX_API size_t HostSize(const void* ptr) noexcept;

/**
 * Returns a snapshot of the counters. The values of different tags are not read at the same
 * instant.
 *
 * @return The counters.
 */
REX_API HostStats GetHostStats() noexcept;

/**
 * Sets how many call stack frames the profiler keeps for each host allocation. Does nothing in a
 * build without profiling.
 *
 * @param depth The frame count, clamped to 0 through 32, 0 for none.
 */
REX_API void SetAllocStackDepth(i32 depth) noexcept;

/**
 * Returns how many call stack frames the profiler keeps for each host allocation.
 *
 * @return The frame count, 0 for none.
 */
[[nodiscard]] REX_API i32 GetAllocStackDepth() noexcept;

/**
 * Returns the tag of the calling thread.
 *
 * @return The tag that new allocations on this thread receive.
 */
REX_API Tag GetThreadTag() noexcept;

/**
 * Returns the name of a tag.
 *
 * @param tag The tag.
 * @return The name, a string with static storage.
 */
REX_API const char* GetTagName(Tag tag) noexcept;

/** Sets the tag of the calling thread for the lifetime of the object. Scopes nest. */
class REX_API TagScope {
 public:
  /**
   * Sets the tag of the calling thread.
   *
   * @param tag The tag that new allocations on this thread receive.
   */
  explicit TagScope(Tag tag) noexcept;

  /** Restores the tag that was set before this scope. */
  ~TagScope();

  TagScope(const TagScope&) = delete;
  TagScope& operator=(const TagScope&) = delete;

 private:
  Tag previous_;
};

namespace detail {

/** Failure a Debug check found in a block that is being freed. */
enum class BlockError : u8 {
  kForeignPointer, /**< The block was not made by HostAlloc, or it was freed already. */
  kKindMismatch, /**< The block was freed with the other form, new[] with delete or the reverse. */
  kSizeMismatch, /**< The size passed to a sized delete differs from the requested size. */
};

/**
 * Receives a Debug check failure. The default handler logs the failure and breaks.
 *
 * @param error The failure.
 * @param ptr The pointer that was freed.
 */
using BlockErrorHandler = void (*)(BlockError error, const void* ptr);

/**
 * Replaces the handler of Debug check failures.
 *
 * @param handler The new handler, null restores the default.
 * @return The previous handler.
 */
REX_API BlockErrorHandler SetBlockErrorHandler(BlockErrorHandler handler) noexcept;

/**
 * Makes the next allocations fail as if the system were out of memory.
 *
 * @param count The number of allocations to fail, 0 stops failing.
 */
REX_API void FailNextAllocations(u32 count) noexcept;

/** Form of the call that allocated or frees a block. */
enum class Kind : u8 {
  kScalar, /**< operator new or operator delete. */
  kArray,  /**< operator new[] or operator delete[]. */
  kRaw,    /**< HostAlloc or HostFree. */
};

/**
 * Allocates for an operator new form. Retries through the new handler and throws on failure.
 *
 * @param size The size in bytes.
 * @param align The alignment in bytes, 0 for the default.
 * @param kind The form of the operator.
 * @param can_throw False for the nothrow forms, which return null on failure.
 * @return The block, or null for a nothrow form that failed.
 */
REX_API void* NewBlock(size_t size, size_t align, Kind kind, bool can_throw);

/**
 * Frees for an operator delete form. Checks the block in Debug.
 *
 * @param ptr The block, null does nothing.
 * @param size The size from a sized delete, 0 when unknown.
 * @param kind The form of the operator.
 */
REX_API void DeleteBlock(void* ptr, size_t size, Kind kind) noexcept;

}  // namespace detail

}  // namespace rex::memory
