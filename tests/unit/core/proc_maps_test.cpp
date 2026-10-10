/**
 * @file        tests/unit/core/proc_maps_test.cpp
 * @brief       The process map reader and the page protection query built on it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/platform.h>

#if REX_PLATFORM_LINUX

#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <rex/memory/utils.h>

#include "core/proc_maps.h"

namespace {

struct Mapping {
  void* address = MAP_FAILED;
  size_t size = 0;

  Mapping(int prot, int fd = -1) : size(static_cast<size_t>(sysconf(_SC_PAGESIZE))) {
    address = mmap(nullptr, size, prot, fd < 0 ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_PRIVATE, fd, 0);
  }
  ~Mapping() {
    if (address != MAP_FAILED) {
      munmap(address, size);
    }
  }
};

rex::memory::PageAccess AccessOf(void* address) {
  size_t length = 0;
  rex::memory::PageAccess access{};
  REQUIRE(rex::memory::QueryProtect(address, length, access));
  CHECK(length > 0);
  return access;
}

}  // namespace

REX_TEST_CASE("QueryProtect reports the protection of read only, read write and no access pages",
              "[core][memory]") {
  const Mapping read_only(PROT_READ);
  const Mapping read_write(PROT_READ | PROT_WRITE);
  const Mapping no_access(PROT_NONE);
  REQUIRE(read_only.address != MAP_FAILED);
  REQUIRE(read_write.address != MAP_FAILED);
  REQUIRE(no_access.address != MAP_FAILED);
  CHECK(AccessOf(read_only.address) == rex::memory::PageAccess::kReadOnly);
  CHECK(AccessOf(read_write.address) == rex::memory::PageAccess::kReadWrite);
  CHECK(AccessOf(no_access.address) == rex::memory::PageAccess::kNoAccess);
}

REX_TEST_CASE(
    "A mapped file with a path longer than the line buffer keeps its fields and the next line",
    "[core][memory]") {
  std::filesystem::path folder = std::filesystem::temp_directory_path() / "rex-proc-maps";
  for (int i = 0; i < 6; ++i) {
    folder /= std::string(200, static_cast<char>('a' + i));
  }
  std::filesystem::create_directories(folder);
  const std::filesystem::path file = folder / "mapped.bin";
  std::ofstream(file, std::ios::binary) << std::string(4096, 'x');
  REQUIRE(file.string().size() > 1024);
  const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  const Mapping mapped(PROT_READ, fd);
  close(fd);
  const Mapping after(PROT_READ | PROT_WRITE);
  REQUIRE(mapped.address != MAP_FAILED);
  REQUIRE(after.address != MAP_FAILED);

  const auto start = reinterpret_cast<rex::u64>(mapped.address);
  bool seen = false;
  rex::memory::ForEachMapping([&](const rex::memory::ProcMapping& mapping) {
    if (mapping.start == start) {
      seen = true;
      CHECK(mapping.end == start + mapped.size);
      CHECK(mapping.perms[0] == 'r');
      CHECK(mapping.perms[1] == '-');
      CHECK(mapping.offset == 0);
      CHECK(file.string().starts_with(mapping.path));
      CHECK(mapping.path.size() < file.string().size());
    }
    return true;
  });
  CHECK(seen);
  CHECK(AccessOf(mapped.address) == rex::memory::PageAccess::kReadOnly);
  CHECK(AccessOf(after.address) == rex::memory::PageAccess::kReadWrite);
  std::filesystem::remove_all(std::filesystem::temp_directory_path() / "rex-proc-maps");
}

#endif
