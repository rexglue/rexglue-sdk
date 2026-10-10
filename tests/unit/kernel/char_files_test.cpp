/**
 * @file        tests/unit/kernel/char_files_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/runtime/info/file.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = rex::X_STATUS_INVALID_HANDLE;
constexpr u32 kReadWriteAccess = 0xC0100000;

void MountScratch(KernelFixture& fixture) {
  namespace fs = rex::filesystem;
  auto* file_system = fixture.api().Get<fs::IFileSystem>();
  REQUIRE(file_system);
  REQUIRE(file_system->Mount({"\\Device\\Scratch", fs::ProviderKind::kHostDirectory,
                              fixture.root() / "scratch", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Link("scratch:", "\\Device\\Scratch"));
}

void WriteScratch(const std::filesystem::path& root, const char* name, std::streamsize size) {
  std::filesystem::create_directories(root / "scratch");
  std::ofstream out(root / "scratch" / name, std::ios::binary);
  const std::string bytes(static_cast<size_t>(size), 'x');
  out.write(bytes.data(), size);
}
}  // namespace

TEST_CASE("files: an allocation size only truncates below the end of file", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "a.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\a.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    c.Store64(info, 4096);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::runtime::XFileAllocationInformation}) == 0);
    c.Store64(info, 8);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::runtime::XFileAllocationInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.root() / "scratch" / "a.bin") == 8);
}

TEST_CASE("files: a flush reaches the file and fails on a bad handle", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "f.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\f.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtFlushBuffersFile", {handle, status_block}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtFlushBuffersFile", {handle, status_block}) == kInvalidHandle);
  });
}

TEST_CASE("files: a rename onto a file needs replace existing", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    WriteScratch(root, "r1.bin", 16);
    WriteScratch(root, "r2.bin", 4);
  });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\r1.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    constexpr std::string_view kTarget = "scratch:\\r2.bin";
    const u32 info = c.Alloc(16);
    c.Store32(info, 0);
    c.Store32(info + 4, 0);
    c.Store32(info + 8, static_cast<u32>(kTarget.size() << 16 | kTarget.size()));
    c.Store32(info + 12, c.String(kTarget));
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 16, rex::runtime::XFileRenameInformation}) ==
          rex::X_STATUS_OBJECT_NAME_COLLISION);
    c.Store32(info, 1);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 16, rex::runtime::XFileRenameInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK_FALSE(std::filesystem::exists(fixture.root() / "scratch" / "r1.bin"));
  CHECK(std::filesystem::file_size(fixture.root() / "scratch" / "r2.bin") == 16);
}

TEST_CASE("files: an allocation size above the end of file keeps the size", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "b.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\b.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    c.Store64(info, 4096);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::runtime::XFileAllocationInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.root() / "scratch" / "b.bin") == 16);
}

TEST_CASE("files: create read write seek and query", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "data.bin", std::ios::binary);
    const char bytes[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\data.bin"),
                                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(16);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(0)}) == 0);
    CHECK(c.Load32(status_block + 4) == 8);
    CHECK(c.Load32(buffer) == 0x00010203);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(8)}) == 0);
    CHECK(c.Load32(buffer) == 0x08090A0B);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(16)}) ==
          rex::X_STATUS_END_OF_FILE);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(0)}) ==
          kInvalidHandle);
  });
}

TEST_CASE("files: missing file", "[char][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECK(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\missing.bin"),
                                    status_block, 0, 0, 3, 1, 0x20}) == rex::X_STATUS_NO_SUCH_FILE);
  });
}

TEST_CASE("io completion: set remove and timeout", "[char][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 port_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateIoCompletion", {port_ptr, 0x1F0003, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 port = c.Load32(port_ptr);
    const u32 key = c.Alloc(4);
    const u32 context = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECK(c.Kernel("NtRemoveIoCompletion", {port, key, context, status_block, c.Quad(0)}) ==
          rex::X_STATUS_TIMEOUT);
    CHECK(c.Kernel("NtSetIoCompletion", {port, 11, 22, 0, 33}) == 0);
    CHECK(c.Kernel("NtRemoveIoCompletion", {port, key, context, status_block, c.Quad(0)}) == 0);
    CHECK(c.Load32(key) == 11);
    CHECK(c.Load32(context) == 22);
    CHECK(c.Load32(status_block + 4) == 33);
    CHECK(c.Kernel("NtClose", {port}) == 0);
  });
}

TEST_CASE("files: sequential reads advance the position", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "seq.bin", std::ios::binary);
    const char bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\seq.bin"),
                                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 4, 0}) == 0);
    CHECK(c.Load32(buffer) == 0x00010203);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 4, 0}) == 0);
    CHECK(c.Load32(buffer) == 0x04050607);
    c.Kernel("NtClose", {handle});
  });
}

TEST_CASE("files: full attributes report the size", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "size.bin", std::ios::binary);
    const char bytes[16] = {};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 info = c.Alloc(0x38);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {c.Attributes("game:\\size.bin"), info}) == 0);
    CHECK(c.Load32(info + 0x2C) == 16);
  });
}

TEST_CASE("files: a creation disposition above overwrite if is an invalid parameter",
          "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "d.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    for (const u32 disposition : {6u, 0x101u}) {
      CHECK(c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\d.bin"),
                                      status_block, 0, 0, 3, disposition, 0x20}) ==
            rex::X_STATUS_INVALID_PARAMETER);
    }
  });
}
