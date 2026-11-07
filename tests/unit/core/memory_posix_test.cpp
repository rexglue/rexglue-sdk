/**
 * @file        memory_posix_test.cpp
 * @brief       POSIX file-mapping lifecycle tests
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cerrno>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>

TEST_CASE("POSIX mapping remains usable after its shared-memory name is removed",
          "[memory][posix]") {
  const std::string name = "rmem_" + std::to_string(getpid());
  const std::filesystem::path path(name);
  constexpr size_t kMappingSize = 4096;

  auto handle = rex::memory::CreateFileMappingHandle(
      path, kMappingSize, rex::memory::PageAccess::kReadWrite, false, true);
  REQUIRE(handle != rex::memory::kFileMappingHandleInvalid);

  errno = 0;
  const int named_handle = shm_open(("/" + name).c_str(), O_RDWR, 0);
  CHECK(named_handle == -1);
  CHECK(errno == ENOENT);
  if (named_handle >= 0) {
    close(named_handle);
  }

  void* view = rex::memory::MapFileView(handle, nullptr, kMappingSize,
                                        rex::memory::PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  auto* value = static_cast<uint32_t*>(view);
  *value = 0x12345678;
  CHECK(*value == 0x12345678);

  CHECK(rex::memory::UnmapFileView(handle, view, kMappingSize));
  rex::memory::CloseFileMappingHandle(handle, path);
}

TEST_CASE("POSIX mapping keeps its shared-memory name by default", "[memory][posix]") {
  const std::string name = "rmem_named_" + std::to_string(getpid());
  const std::filesystem::path path(name);
  constexpr size_t kMappingSize = 4096;

  auto handle = rex::memory::CreateFileMappingHandle(path, kMappingSize,
                                                     rex::memory::PageAccess::kReadWrite, false);
  REQUIRE(handle != rex::memory::kFileMappingHandleInvalid);

  const int named_handle = shm_open(("/" + name).c_str(), O_RDWR, 0);
  CHECK(named_handle >= 0);
  if (named_handle >= 0) {
    close(named_handle);
  }

  rex::memory::CloseFileMappingHandle(handle, path);
  CHECK(shm_open(("/" + name).c_str(), O_RDWR, 0) == -1);
  CHECK(errno == ENOENT);
}
