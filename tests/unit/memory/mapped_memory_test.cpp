/**
 * @file        tests/unit/memory/mapped_memory_test.cpp
 * @brief       MappedMemory ranges are non owning views, chunked writers spill to new files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"
#include "file_text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>

#include <rex/memory/mapped_memory.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

using rex::memory::ChunkedMappedMemoryWriter;
using rex::memory::MappedMemory;

namespace {
using rex::test::ReadBytes;
}  // namespace

static_assert(std::is_constructible_v<MappedMemory, void*, size_t>);

REX_TEST_CASE("Range returns a non owning view of the same bytes", "[memory][mapped_memory]") {
  std::array<rex::u8, 16> bytes{};
  MappedMemory mapping(bytes.data(), bytes.size());
  auto view = mapping.Range(4, 8);
  REQUIRE(view != nullptr);
  CHECK(view->GetData() == bytes.data() + 4);
  CHECK(view->GetSize() == 8);
  view.reset();
  CHECK(mapping.GetData() == bytes.data());
}

REX_TEST_CASE("A chunked writer spills a full chunk into the next file",
              "[memory][mapped_memory]") {
  const rex::test::TempFolder folder("rex_chunked_writer_test_");
  const auto& dir = folder.path;
  const size_t chunk_size = rex::memory::AllocationGranularity();
  const size_t head = chunk_size - 16;
  const size_t tail = 64;
  {
    auto writer = ChunkedMappedMemoryWriter::Open(dir / "trace", chunk_size);
    REQUIRE(writer != nullptr);
    rex::u8* first = writer->Allocate(head);
    REQUIRE(first != nullptr);
    std::fill_n(first, head, rex::u8{0xA5});
    rex::u8* second = writer->Allocate(tail);
    REQUIRE(second != nullptr);
    std::fill_n(second, tail, rex::u8{0x5A});
    writer->Flush();
  }
  const auto chunk_0 = ReadBytes(dir / "trace.0");
  const auto chunk_1 = ReadBytes(dir / "trace.1");
  REQUIRE(chunk_0.size() == chunk_size);
  REQUIRE(chunk_1.size() == chunk_size);
  CHECK(std::all_of(chunk_0.begin(), chunk_0.begin() + head, [](rex::u8 b) { return b == 0xA5; }));
  CHECK(std::all_of(chunk_1.begin(), chunk_1.begin() + tail, [](rex::u8 b) { return b == 0x5A; }));
}

REX_TEST_CASE("A low address chunked writer maps below 4 GiB", "[memory][mapped_memory]") {
  const rex::test::TempFolder folder("rex_chunked_writer_low_test_");
  const auto& dir = folder.path;
  {
    auto writer =
        ChunkedMappedMemoryWriter::Open(dir / "trace", rex::memory::AllocationGranularity(), true);
    REQUIRE(writer != nullptr);
#if REX_PLATFORM_MAC
    CHECK(writer->Allocate(16) == nullptr);
#else
    rex::u8* data = writer->Allocate(16);
    REQUIRE(data != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(data) < (uintptr_t{1} << 32));
#endif
  }
}

#if REX_PLATFORM_WIN32
REX_TEST_CASE("A low address chunked writer moves past an occupied first address",
              "[memory][mapped_memory]") {
  const rex::test::TempFolder folder("rex_chunked_writer_busy_test_");
  const auto& dir = folder.path;
  const size_t chunk_size = rex::memory::AllocationGranularity();
  // NOLINTNEXTLINE(performance-no-int-to-ptr): VirtualAlloc reserves at this fixed address
  void* first_address = reinterpret_cast<void*>(uintptr_t{0x10000000});
  void* blocker = VirtualAlloc(first_address, chunk_size, MEM_RESERVE, PAGE_NOACCESS);
  {
    auto writer = ChunkedMappedMemoryWriter::Open(dir / "trace", chunk_size, true);
    REQUIRE(writer != nullptr);
    rex::u8* data = writer->Allocate(16);
    REQUIRE(data != nullptr);
    CHECK(data != first_address);
    CHECK(reinterpret_cast<uintptr_t>(data) < (uintptr_t{1} << 32));
  }
  if (blocker) {
    VirtualFree(blocker, 0, MEM_RELEASE);
  }
}
#endif
