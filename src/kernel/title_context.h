/**
 * @file        kernel/title_context.h
 * @brief       Title context of a TitleRuntime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <condition_variable>
#include <mutex>

#include <rex/platform.h>
#include <rex/title.h>
#include <rex/guest.h>
#include <rex/utility.h>

namespace rex::kernel {

class TitleRuntime;

/** The physical memory, write watch, guest interrupts and achievements of a TitleRuntime. */
class TitleContext final : public IPhysicalMemory,
                           public IPhysicalWriteWatch,
                           public IGuestInterrupts,
                           public IAchievementRegistry {
 public:
  /**
   * @param runtime The runtime, which must outlive the context.
   */
  explicit TitleContext(TitleRuntime& runtime);
  ~TitleContext() override;

  /** Waits until every handle the context gave out is destroyed. */
  void Detach();

  [[nodiscard]] Result<void> CopyFromPhysical(u32 address, std::span<std::byte> out) override;
  [[nodiscard]] Result<void> CopyToPhysical(u32 address, std::span<const std::byte> data) override;
  [[nodiscard]] Result<std::span<const std::byte>> GetReadOnlyPhysicalSpan(
      PhysicalRange range) override;
  [[nodiscard]] Result<std::span<std::byte>> GetWritablePhysicalSpan(PhysicalRange range) override;
  [[nodiscard]] Result<void> StoreRelease32(u32 address, u32 value) override;
  [[nodiscard]] Result<u32> LoadAcquire32(u32 address) override;
  [[nodiscard]] ScopeGuard AddWriteListener(IPhysicalWriteListener& listener) override;
  void WatchPhysicalWrites(PhysicalRange range) override;
  [[nodiscard]] ScopeGuard LockWriteWatches() override;
  [[nodiscard]] Result<void> RaiseGraphicsInterrupt(u32 source, u32 cpu) override;
  [[nodiscard]] Result<ScopeGuard> StartHostThread(
      std::string_view name, MoveOnlyFunction<void(std::stop_token)> fn) override;
  std::vector<Achievement> GetAchievements() override;
  [[nodiscard]] Result<std::vector<std::byte>> GetAchievementIcon(u32 image_id) override;
  [[nodiscard]] ScopeGuard OnAchievementUnlocked(
      MoveOnlyFunction<void(const Achievement&)> fn) override;

 private:
  void AddHandle();
  void ReleaseHandle();

  TitleRuntime& runtime_;
  std::mutex handle_mutex_;
  std::condition_variable handle_released_;
  u32 live_handles_ = 0;
};

}  // namespace rex::kernel
