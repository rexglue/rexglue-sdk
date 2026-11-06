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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include <rex/memory/host.h>

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
