/**
 * @file        kernel/guest_allocator.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <mutex>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class GuestMemory;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel::shim {
class GuestHeap;
}  // namespace rex::kernel::shim

namespace rex::kernel {

class GuestAddressSpace;
class WorkspaceHeap;

/** Profiler pool that the blocks of a guest allocator are reported to. */
enum class GuestPool : u8 {
  kHeap, /**< The shim Rtl heap. */
  kWorkspace,
  kTitle,
  kCount, /**< Number of pools, not a pool. */
};

/** Reserves and commits a range of guest memory and serves blocks from it. */
class REX_KERNEL_API GuestAllocator : public NonCopyable {
  /** Limits construction to Create. */
  struct Key {};

 public:
  /** Alignment of every block in bytes. */
  static constexpr u32 kBlockAlignment = 16;

  struct AllocatorStats {
    u64 capacity;
    u64 allocated;
    u64 peak_allocated;
    u64 peak_request_size;
    u64 oom_count;
  };

  /**
   * Wraps a committed range. Only Create can make the key.
   *
   * @param state The allocator state that Create built over the range.
   * @param pool The pool the blocks are reported to.
   */
  GuestAllocator(Key, GuestAddressSpace& memory, u32 base, u32 size, void* state, GuestPool pool);

  /**
   * Reserves and commits a range and starts the allocator over it.
   *
   * @param memory Guest memory, which must outlive the allocator.
   * @param physical True to take the range from the physical heap, false for the virtual heap.
   * @param page_size The page size that selects the heap, in bytes.
   * @param flags Allocation type flags added to reserve and commit.
   * @param pool The pool the blocks are reported to.
   * @return An error when the range cannot be committed or the allocator cannot start.
   */
  [[nodiscard]] static Result<std::unique_ptr<GuestAllocator>> Create(
      rex::kernel::GuestMemory& memory, u32 size, u32 alignment, bool physical, u32 page_size,
      u32 flags, GuestPool pool);

  /** Returns the name of the profiler graph that plots a pool, a string with static storage. */
  [[nodiscard]] static const char* GetPoolPlotName(GuestPool pool);

  ~GuestAllocator();

  [[nodiscard]] u32 GetBase() const;

  [[nodiscard]] u32 GetSize() const;

  [[nodiscard]] GuestPool GetPool() const;

  [[nodiscard]] bool Contains(u32 address) const;

  /**
   * Allocates a block.
   *
   * @return The host pointer of the block, or null when the range is exhausted.
   */
  [[nodiscard]] void* Allocate(u32 size);

  /**
   * Resizes a block, moving it when it cannot grow in place.
   *
   * @param block A pointer from Allocate.
   * @return The host pointer of the resized block, or null when the range is exhausted and the
   * block is left as it was.
   */
  [[nodiscard]] void* Reallocate(void* block, u32 size);

  /**
   * Frees a block.
   *
   * @param block A pointer from Allocate, null for none.
   */
  void Free(void* block);

  [[nodiscard]] AllocatorStats GetStats() const;

  [[nodiscard]] bool IsConsistent() const;

 private:
  friend class WorkspaceHeap;
  friend class rex::kernel::shim::GuestHeap;

  [[nodiscard]] static Result<std::unique_ptr<GuestAllocator>> Create(GuestAddressSpace& memory,
                                                                      u32 size, u32 alignment,
                                                                      bool physical, u32 page_size,
                                                                      u32 flags, GuestPool pool);

  GuestAddressSpace& memory_;
  u32 base_;
  u32 size_;
  void* state_;
  GuestPool pool_;
  mutable std::mutex mutex_;
  u64 capacity_ = 0;
  u64 allocated_ = 0;
  u64 peak_allocated_ = 0;
  u64 peak_request_size_ = 0;
  u64 oom_count_ = 0;
};

}  // namespace rex::kernel

namespace rex::kernel {

class GuestAddressSpace;

/** Interface XAM uses to allocate from the title workspace. */
class ITitleWorkspace {
 public:
  virtual ~ITitleWorkspace() = default;

  /**
   * Allocates bytes from the workspace.
   *
   * @param zero True to zero the bytes.
   * @return The guest address, or an error when the workspace is exhausted.
   */
  [[nodiscard]] virtual Result<u32> Allocate(u32 size, bool zero) = 0;

  /**
   * Frees an address from Allocate. An address outside the workspace is logged and ignored.
   *
   * @param address The guest address, 0 for none.
   */
  virtual void Free(u32 address) = 0;
};

/** The title workspace, title memory the system uses on the title's behalf. */
class REX_KERNEL_API WorkspaceHeap : public ITitleWorkspace, public NonCopyable {
  /** Limits construction to Create. */
  struct Key {};

 public:
  /** Wraps a committed workspace. Only Create can make the key. */
  WorkspaceHeap(Key, GuestAddressSpace& memory, std::unique_ptr<GuestAllocator> allocator);

  /**
   * @return An error when the memory cannot be committed.
   */
  [[nodiscard]] static Result<std::unique_ptr<WorkspaceHeap>> Create(GuestAddressSpace& memory,
                                                                     u32 size);

  /**
   * Resolves the workspace size of a title.
   *
   * @param header_size The XEX workspace size, 0 when the header is missing.
   */
  [[nodiscard]] static u32 ResolveSize(u32 header_size);

  [[nodiscard]] u32 GetSize() const;

  /**
   * Allocates bytes from the workspace.
   *
   * @param zero True to zero the bytes.
   * @return The guest address, or an error when the workspace is exhausted.
   */
  [[nodiscard]] Result<u32> Allocate(u32 size, bool zero) override;

  /**
   * Frees an address from Allocate. An address outside the workspace is logged and ignored.
   *
   * @param address The guest address, 0 for none.
   */
  void Free(u32 address) override;

 private:
  GuestAddressSpace& memory_;
  std::unique_ptr<GuestAllocator> allocator_;
};

}  // namespace rex::kernel
