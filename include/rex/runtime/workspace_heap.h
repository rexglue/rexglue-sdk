/**
 * @file        runtime/workspace_heap.h
 * @brief       Title workspace, title memory the system allocates on the title's behalf
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

#include <rex/result.h>
#include <rex/types.h>

struct O1HeapInstance;

namespace rex::runtime {

class Memory;

/** The title workspace, title memory the system uses on the title's behalf. */
class WorkspaceHeap : public NonCopyable {
 public:
  /**
   * Commits the workspace.
   *
   * @param memory Guest memory.
   * @param size Workspace bytes.
   * @return An error when the memory cannot be committed.
   */
  [[nodiscard]] static Result<std::unique_ptr<WorkspaceHeap>> Create(Memory& memory, u32 size);
  ~WorkspaceHeap();

  /**
   * Resolves the workspace size of a title.
   *
   * @param header_size The XEX workspace size, 0 when the header is missing.
   * @return The size in bytes.
   */
  [[nodiscard]] static u32 ResolveSize(u32 header_size);

  /**
   * Returns the workspace size.
   *
   * @return The size in bytes.
   */
  [[nodiscard]] u32 size() const;

  /**
   * Allocates zeroed bytes from the workspace.
   *
   * @param size Bytes.
   * @return The guest address, or an error when the workspace is exhausted.
   */
  [[nodiscard]] Result<u32> Alloc(u32 size);

  /**
   * Frees an address from Alloc. An address outside the workspace is logged and ignored.
   *
   * @param address The guest address, 0 for none.
   */
  void Free(u32 address);

 private:
  WorkspaceHeap(Memory& memory, u32 base, u32 size, O1HeapInstance* heap);

  Memory& memory_;
  u32 base_;
  u32 size_;
  O1HeapInstance* heap_;
  std::mutex mutex_;
};

}  // namespace rex::runtime
