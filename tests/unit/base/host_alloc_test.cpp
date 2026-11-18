/**
 * @file        tests/unit/base/host_alloc_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <new>
#include <thread>
#include <vector>

#include <rex/memory/host.h>

#include "benchmark_pool.h"

using rex::memory::GetHostStats;
using rex::memory::HostAlloc;
using rex::memory::HostFree;
using rex::memory::HostSize;
using rex::memory::Tag;
using rex::memory::TagScope;

namespace {

void* g_early_block = HostAlloc(10);

constexpr Tag kFirstTag{12};
constexpr Tag kSecondTag{13};
constexpr Tag kThirdTag{14};
constexpr Tag kUnnamedTag{15};

constexpr size_t Index(Tag tag) {
  return static_cast<size_t>(tag);
}

rex::memory::detail::BlockError g_last_error;
const void* g_last_error_ptr = nullptr;
int g_error_count = 0;

void OnBlockError(rex::memory::detail::BlockError error, const void* ptr) {
  g_last_error = error;
  g_last_error_ptr = ptr;
  ++g_error_count;
}

struct ErrorCapture {
  ErrorCapture() {
    g_error_count = 0;
    previous = rex::memory::detail::SetBlockErrorHandler(&OnBlockError);
  }
  ~ErrorCapture() { rex::memory::detail::SetBlockErrorHandler(previous); }
  rex::memory::detail::BlockErrorHandler previous;
};

int g_handler_calls = 0;

void CountingHandler() {
  ++g_handler_calls;
}

}  // namespace

REX_TEST_CASE("A block allocated before main is usable and freed normally", "[base][memory]") {
  REQUIRE(g_early_block != nullptr);
  CHECK(HostSize(g_early_block) == 10);
  HostFree(g_early_block);
  g_early_block = nullptr;
}

REX_TEST_CASE("SetAllocStackDepth stores the depth and clamps it", "[base][memory]") {
  using rex::memory::GetAllocStackDepth;
  using rex::memory::SetAllocStackDepth;
  CHECK(GetAllocStackDepth() == 0);
  SetAllocStackDepth(12);
  CHECK(GetAllocStackDepth() == 12);
  SetAllocStackDepth(1000);
  CHECK(GetAllocStackDepth() == 32);
  SetAllocStackDepth(-3);
  CHECK(GetAllocStackDepth() == 0);
  SetAllocStackDepth(4);
  void* block = rex::memory::HostAlloc(64);
  REQUIRE(block != nullptr);
  rex::memory::HostFree(block);
  SetAllocStackDepth(0);
}

REX_TEST_CASE("HostAlloc honors sizes and alignments", "[base][memory]") {
  for (const size_t align : {size_t{0}, size_t{1}, size_t{8}, size_t{16}, size_t{32}, size_t{64},
                             size_t{256}, size_t{4096}}) {
    for (const size_t size : {size_t{0}, size_t{1}, size_t{15}, size_t{16}, size_t{17}, size_t{255},
                              size_t{4096}, size_t{1} << 20}) {
      void* block = HostAlloc(size, align);
      REQUIRE(block != nullptr);
      const size_t required = align > 16 ? align : 16;
      CHECK(reinterpret_cast<uintptr_t>(block) % required == 0);
      CHECK(HostSize(block) == size);
      if (size != 0) {
        std::memset(block, 0x5A, size);
      }
      HostFree(block, size, align);
    }
  }
}

REX_TEST_CASE("HostAlloc returns distinct blocks for size 0", "[base][memory]") {
  void* first = HostAlloc(0);
  void* second = HostAlloc(0);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first != second);
  HostFree(first);
  HostFree(second);
}

REX_TEST_CASE("HostAlloc rejects an alignment that is not a power of 2 and a size too large",
              "[base][memory]") {
  CHECK(HostAlloc(16, 24) == nullptr);
  CHECK(HostAlloc(SIZE_MAX, 0) == nullptr);
  CHECK(HostAlloc(SIZE_MAX - 8, 64) == nullptr);
  HostFree(nullptr);
}

REX_TEST_CASE("Counters follow the tag of the allocating thread", "[base][memory]") {
  const auto before = GetHostStats();
  void* block;
  {
    TagScope scope(kFirstTag);
    block = HostAlloc(1000);
  }
  const auto during = GetHostStats();
  REQUIRE(block != nullptr);
  CHECK(during.tags[Index(kFirstTag)].live_bytes ==
        before.tags[Index(kFirstTag)].live_bytes + 1000);
  CHECK(during.tags[Index(kFirstTag)].live_count == before.tags[Index(kFirstTag)].live_count + 1);
  CHECK(during.tags[Index(kFirstTag)].total_count == before.tags[Index(kFirstTag)].total_count + 1);
  CHECK(during.tags[Index(kFirstTag)].peak_bytes >= during.tags[Index(kFirstTag)].live_bytes);
  HostFree(block);
  const auto after = GetHostStats();
  CHECK(after.tags[Index(kFirstTag)].live_bytes == before.tags[Index(kFirstTag)].live_bytes);
  CHECK(after.tags[Index(kFirstTag)].live_count == before.tags[Index(kFirstTag)].live_count);
  CHECK(after.tags[Index(kFirstTag)].peak_bytes >= before.tags[Index(kFirstTag)].peak_bytes + 1000);
}

REX_TEST_CASE("TagScope nests and restores the previous tag", "[base][memory]") {
  CHECK(rex::memory::GetThreadTag() == Tag::kCore);
  {
    TagScope outer(kFirstTag);
    CHECK(rex::memory::GetThreadTag() == kFirstTag);
    {
      TagScope inner(kSecondTag);
      CHECK(rex::memory::GetThreadTag() == kSecondTag);
    }
    CHECK(rex::memory::GetThreadTag() == kFirstTag);
  }
  CHECK(rex::memory::GetThreadTag() == Tag::kCore);
}

REX_TEST_CASE("A tag is private to its thread", "[base][memory]") {
  TagScope scope(kThirdTag);
  Tag seen = kThirdTag;
  std::thread thread([&seen] { seen = rex::memory::GetThreadTag(); });
  thread.join();
  CHECK(seen == Tag::kCore);
  CHECK(rex::memory::GetThreadTag() == kThirdTag);
}

REX_TEST_CASE("A block freed on another thread returns to the tag that allocated it",
              "[base][memory]") {
  const auto before = GetHostStats();
  void* block;
  {
    TagScope scope(kFirstTag);
    block = HostAlloc(500);
  }
  std::thread thread([block] {
    TagScope scope(kSecondTag);
    HostFree(block);
  });
  thread.join();
  const auto after = GetHostStats();
  CHECK(after.tags[Index(kFirstTag)].live_bytes == before.tags[Index(kFirstTag)].live_bytes);
  CHECK(after.tags[Index(kSecondTag)].live_bytes == before.tags[Index(kSecondTag)].live_bytes);
}

REX_TEST_CASE("The total counters add up the tags", "[base][memory]") {
  const auto before = GetHostStats();
  void* first;
  void* second;
  {
    TagScope scope(kThirdTag);
    first = HostAlloc(300);
  }
  {
    TagScope scope(Tag::kThirdParty);
    second = HostAlloc(200);
  }
  const auto during = GetHostStats();
  CHECK(during.total.live_bytes >= before.total.live_bytes + 500);
  CHECK(during.total.peak_bytes >= during.total.live_bytes);
  HostFree(first);
  HostFree(second);
}

REX_TEST_CASE("A tag reads the name a module gave it", "[base][memory]") {
  rex::memory::SetTagName(kFirstTag, "First");
  CHECK(std::strcmp(rex::memory::GetTagName(kFirstTag), "First") == 0);
  rex::memory::SetTagName(kFirstTag, "Renamed");
  CHECK(std::strcmp(rex::memory::GetTagName(kFirstTag), "Renamed") == 0);
  CHECK(std::strcmp(rex::memory::GetTagName(Tag::kCore), "Core") == 0);
  CHECK(std::strcmp(rex::memory::GetTagName(Tag::kThirdParty), "ThirdParty") == 0);
}

REX_TEST_CASE("An unnamed or out of range tag reads Invalid and counts nothing", "[base][memory]") {
  CHECK(std::strcmp(rex::memory::GetTagName(kUnnamedTag), "Invalid") == 0);
  CHECK(std::strcmp(rex::memory::GetTagName(static_cast<Tag>(200)), "Invalid") == 0);
  rex::memory::SetTagName(static_cast<Tag>(200), "Ignored");
  CHECK(std::strcmp(rex::memory::GetTagName(static_cast<Tag>(200)), "Invalid") == 0);
  const auto stats = GetHostStats();
  CHECK(stats.tags[Index(kUnnamedTag)].total_count == 0);
}

REX_TEST_CASE("A scope with an out of range tag uses the core tag", "[base][memory]") {
  TagScope scope(static_cast<Tag>(200));
  CHECK(rex::memory::GetThreadTag() == Tag::kCore);
}

REX_TEST_CASE("Failed allocations return null, throw or retry through the new handler",
              "[base][memory]") {
  rex::memory::detail::FailNextAllocations(1);
  CHECK(HostAlloc(64) == nullptr);
  void* block = HostAlloc(64);
  CHECK(block != nullptr);
  HostFree(block);

  using rex::memory::detail::AllocType;
  rex::memory::detail::FailNextAllocations(1);
  CHECK_THROWS_AS(rex::memory::detail::NewBlock(64, 0, AllocType::kScalar, true), std::bad_alloc);

  rex::memory::detail::FailNextAllocations(1);
  CHECK(rex::memory::detail::NewBlock(64, 0, AllocType::kScalar, false) == nullptr);

  g_handler_calls = 0;
  const std::new_handler previous = std::set_new_handler(&CountingHandler);
  rex::memory::detail::FailNextAllocations(2);
  void* retried = rex::memory::detail::NewBlock(64, 0, AllocType::kScalar, true);
  std::set_new_handler(previous);
  CHECK(retried != nullptr);
  CHECK(g_handler_calls == 2);
  rex::memory::detail::DeleteBlock(retried, 64, AllocType::kScalar);
}

#ifndef NDEBUG

REX_TEST_CASE("Debug checks catch a pointer the allocator did not make", "[base][memory]") {
  ErrorCapture capture;
  void* foreign = std::malloc(64);
  REQUIRE(foreign != nullptr);
  HostFree(foreign);
  CHECK(g_error_count == 1);
  CHECK(g_last_error == rex::memory::detail::BlockError::kForeignPointer);
  CHECK(g_last_error_ptr == foreign);
  std::free(foreign);
}

REX_TEST_CASE("Debug checks catch a pointer that is not aligned", "[base][memory]") {
  ErrorCapture capture;
  rex::u8 bytes[64] = {};
  HostFree(bytes + 1);
  CHECK(g_error_count == 1);
  CHECK(g_last_error == rex::memory::detail::BlockError::kForeignPointer);
}

REX_TEST_CASE("Debug checks catch new[] freed as new and the reverse", "[base][memory]") {
  using rex::memory::detail::AllocType;
  ErrorCapture capture;
  void* array = rex::memory::detail::NewBlock(32, 0, AllocType::kArray, true);
  rex::memory::detail::DeleteBlock(array, 0, AllocType::kScalar);
  CHECK(g_error_count == 1);
  CHECK(g_last_error == rex::memory::detail::BlockError::kTypeMismatch);
  rex::memory::detail::DeleteBlock(array, 0, AllocType::kArray);
  CHECK(g_error_count == 1);

  void* scalar = rex::memory::detail::NewBlock(32, 0, AllocType::kScalar, true);
  rex::memory::detail::DeleteBlock(scalar, 0, AllocType::kArray);
  CHECK(g_error_count == 2);
  rex::memory::detail::DeleteBlock(scalar, 0, AllocType::kScalar);
  CHECK(g_error_count == 2);
}

REX_TEST_CASE("Debug checks accept the raw form with either operator form", "[base][memory]") {
  using rex::memory::detail::AllocType;
  ErrorCapture capture;
  void* raw = HostAlloc(32);
  rex::memory::detail::DeleteBlock(raw, 0, AllocType::kArray);
  void* scalar = rex::memory::detail::NewBlock(32, 0, AllocType::kScalar, true);
  HostFree(scalar);
  CHECK(g_error_count == 0);
}

REX_TEST_CASE("Debug checks catch a sized delete with the wrong size", "[base][memory]") {
  using rex::memory::detail::AllocType;
  ErrorCapture capture;
  void* block = rex::memory::detail::NewBlock(48, 0, AllocType::kScalar, true);
  rex::memory::detail::DeleteBlock(block, 40, AllocType::kScalar);
  CHECK(g_error_count == 1);
  CHECK(g_last_error == rex::memory::detail::BlockError::kSizeMismatch);
  rex::memory::detail::DeleteBlock(block, 48, AllocType::kScalar);
  CHECK(g_error_count == 1);
}

REX_TEST_CASE("Debug builds fill new and freed memory", "[base][memory]") {
  auto* bytes = static_cast<rex::u8*>(HostAlloc(32));
  REQUIRE(bytes != nullptr);
  CHECK(bytes[0] == 0xCD);
  CHECK(bytes[31] == 0xCD);
  HostFree(bytes);
}

#endif

REX_TEST_CASE("Counters from many threads sum to every allocation", "[base][memory][threads]") {
  constexpr size_t kThreads = 8;
  constexpr size_t kPerThread = 20000;
  constexpr size_t kBlock = 48;
  const auto before = GetHostStats();
  {
    std::latch start(kThreads);
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
      threads.emplace_back([&start] {
        TagScope scope(kSecondTag);
        start.arrive_and_wait();
        for (size_t i = 0; i < kPerThread; ++i) {
          void* block = HostAlloc(kBlock);
          HostFree(block);
        }
      });
    }
  }
  const auto after = GetHostStats();
  const auto& was = before.tags[Index(kSecondTag)];
  const auto& now = after.tags[Index(kSecondTag)];
  CHECK(now.total_count == was.total_count + kThreads * kPerThread);
  CHECK(now.live_bytes == was.live_bytes);
  CHECK(now.live_count == was.live_count);
  CHECK(now.peak_bytes >= was.peak_bytes);
}

REX_TEST_CASE("A block freed by another thread is counted live until then", "[base][memory]") {
  const auto before = GetHostStats();
  constexpr size_t kBlocks = 5000;
  std::vector<void*> blocks;
  blocks.reserve(kBlocks);
  {
    TagScope scope(kThirdTag);
    for (size_t i = 0; i < kBlocks; ++i) {
      blocks.push_back(HostAlloc(100));
    }
  }
  const auto during = GetHostStats();
  CHECK(during.tags[Index(kThirdTag)].live_bytes ==
        before.tags[Index(kThirdTag)].live_bytes + kBlocks * 100);
  CHECK(during.tags[Index(kThirdTag)].peak_bytes >= during.tags[Index(kThirdTag)].live_bytes);
  std::jthread freeing([&blocks] {
    for (void* block : blocks) {
      HostFree(block);
    }
  });
  freeing.join();
  const auto after = GetHostStats();
  CHECK(after.tags[Index(kThirdTag)].live_bytes == before.tags[Index(kThirdTag)].live_bytes);
  CHECK(after.tags[Index(kThirdTag)].live_count == before.tags[Index(kThirdTag)].live_count);
}

namespace {

constexpr size_t kPairs = 100000;

/** Allocates and frees 64 bytes kPairs times with the host allocator. */
void HostPairs() {
  for (size_t i = 0; i < kPairs; ++i) {
    void* block = HostAlloc(64);
    Catch::Benchmark::keep_memory(block);
    HostFree(block);
  }
}

/** Allocates and frees 64 bytes kPairs times with malloc and free. */
void MallocPairs() {
  for (size_t i = 0; i < kPairs; ++i) {
    void* block = std::malloc(64);
    Catch::Benchmark::keep_memory(block);
    std::free(block);
  }
}

}  // namespace

REX_TEST_CASE("host allocator cost", "[base][memory][benchmark]") {
  using rex::test::ThreadPool;
  // Every case runs on threads started before the timed rounds and released together, so the
  // host allocator and malloc are timed the same way and the thread start is in none of them.
  ThreadPool idle_one(1, [](int, rex::test::Caller*) {});
  ThreadPool idle_eight(8, [](int, rex::test::Caller*) {});
  ThreadPool host_one(1, [](int, rex::test::Caller*) { HostPairs(); });
  ThreadPool host_eight(8, [](int, rex::test::Caller*) { HostPairs(); });
  ThreadPool malloc_one(1, [](int, rex::test::Caller*) { MallocPairs(); });
  ThreadPool malloc_eight(8, [](int, rex::test::Caller*) { MallocPairs(); });
  BENCHMARK("baseline: 1 thread released and joined, 0 pairs") {
    idle_one.Run();
  };
  BENCHMARK("baseline: 8 threads released and joined, 0 pairs") {
    idle_eight.Run();
  };
  BENCHMARK("HostAlloc and HostFree of 64 bytes, 1 thread, 100000 pairs") {
    host_one.Run();
  };
  BENCHMARK("HostAlloc and HostFree of 64 bytes, 8 threads, 100000 pairs each") {
    host_eight.Run();
  };
  WARN("8 thread HostAlloc rounds ran together for " << host_eight.GetOverlap() * 100.0
                                                     << " percent of the round");
  BENCHMARK("malloc and free of 64 bytes, 1 thread, 100000 pairs") {
    malloc_one.Run();
  };
  BENCHMARK("malloc and free of 64 bytes, 8 threads, 100000 pairs each") {
    malloc_eight.Run();
  };
}
