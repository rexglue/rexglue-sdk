/**
 * @file        tests/unit/gpu/fake_title_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/kernel/heap.h>
#include <rex/title.h>
#include <rex/guest.h>
#include <rex/utility.h>

namespace rex::test {

/** Serves physical memory from a Memory and logs fence writes. The rest does nothing. */
class FakeTitleContext final : public IPhysicalMemory,
                               public IPhysicalWriteWatch,
                               public IGuestInterrupts,
                               public IAchievementRegistry {
 public:
  explicit FakeTitleContext(kernel::Memory& memory) : memory_(memory) {}

  Result<void> CopyFromPhysical(u32 address, std::span<std::byte> out) override {
    std::memcpy(out.data(), memory_.TranslatePhysical<const u8*>(address), out.size());
    return {};
  }
  Result<void> CopyToPhysical(u32 address, std::span<const std::byte> data) override {
    std::memcpy(memory_.TranslatePhysical<u8*>(address), data.data(), data.size());
    return {};
  }
  Result<std::span<const std::byte>> GetReadOnlyPhysicalSpan(PhysicalRange range) override {
    return std::span<const std::byte>(memory_.TranslatePhysical<const std::byte*>(range.address),
                                      range.length);
  }
  Result<std::span<std::byte>> GetWritablePhysicalSpan(PhysicalRange range) override {
    return std::span<std::byte>(memory_.TranslatePhysical<std::byte*>(range.address), range.length);
  }
  Result<void> StoreRelease32(u32 address, u32 value) override {
    {
      std::lock_guard lock(stores_mutex_);
      stores_.push_back({address, value});
    }
    memory_.TranslatePhysical<be_u32*>(address)->Store(value, std::memory_order_release);
    return {};
  }
  Result<u32> LoadAcquire32(u32 address) override {
    return memory_.TranslatePhysical<const be_u32*>(address)->Load(std::memory_order_acquire);
  }
  ScopeGuard AddWriteListener(IPhysicalWriteListener& listener) override {
    listeners.push_back(&listener);
    return ScopeGuard();
  }
  void WatchPhysicalWrites(PhysicalRange) override {}
  ScopeGuard LockWriteWatches() override { return ScopeGuard(); }
  Result<void> RaiseGraphicsInterrupt(u32, u32) override { return {}; }
  Result<ScopeGuard> StartHostThread(std::string_view,
                                     MoveOnlyFunction<void(std::stop_token)> fn) override {
    auto thread = std::make_shared<std::jthread>(std::move(fn));
    return ScopeGuard([thread] {
      thread->request_stop();
      thread->join();
    });
  }
  std::vector<Achievement> GetAchievements() override { return {}; }
  Result<std::vector<std::byte>> GetAchievementIcon(u32) override { return {}; }
  ScopeGuard OnAchievementUnlocked(MoveOnlyFunction<void(const Achievement&)>) override {
    return ScopeGuard();
  }

  struct Store {
    u32 address;
    u32 value;
  };
  std::vector<Store> GetStores() {
    std::lock_guard lock(stores_mutex_);
    return stores_;
  }
  std::vector<IPhysicalWriteListener*> listeners;

 private:
  kernel::Memory& memory_;
  std::mutex stores_mutex_;
  std::vector<Store> stores_;
};

}  // namespace rex::test
