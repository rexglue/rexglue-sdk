/**
 * @file        tests/unit/kernel/char_files_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/runtime.h>
#include <rex/runtime/info/file.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/file_object.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/xcontent.h>
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

std::atomic<u32> g_apc_calls;
std::atomic<u32> g_apc_context;
std::atomic<u32> g_apc_status_block;

u32 RecordingAPC(Caller& c) {
  g_apc_calls = 0;
  g_apc_context = 0xFFFFFFFF;
  g_apc_status_block = 0;
  return c.runtime().function_dispatcher()->AllocateThunk(
      +[](PPCContext& ctx, u8*) {
        ++g_apc_calls;
        g_apc_context = ctx.r3.u32;
        g_apc_status_block = ctx.r4.u32;
        ctx.r3.u64 = 0;
      },
      0);
}

u32 NewEvent(Caller& c) {
  const u32 event_ptr = c.Alloc(4);
  REQUIRE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
  return c.Load32(event_ptr);
}

u32 DeliverAPCs(Caller& c) {
  const u32 event = NewEvent(c);
  const u32 result = c.Kernel("NtWaitForSingleObjectEx", {event, 1, 1, c.Quad(-10000)});
  c.Kernel("NtClose", {event});
  return result;
}
}  // namespace

REX_TEST_CASE("files: an allocation size only truncates below the end of file", "[char][files]") {
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

REX_TEST_CASE("files: a flush reaches the file and fails on a bad handle", "[char][files]") {
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

REX_TEST_CASE("files: a rename onto a file needs replace existing", "[char][files]") {
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

REX_TEST_CASE("files: an allocation size above the end of file keeps the size", "[char][files]") {
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

REX_TEST_CASE("files: create read write seek and query", "[char][files]") {
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

REX_TEST_CASE("files: missing file", "[char][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECK(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\missing.bin"),
                                    status_block, 0, 0, 3, 1, 0x20}) == rex::X_STATUS_NO_SUCH_FILE);
  });
}

REX_TEST_CASE("io completion: set remove and timeout", "[char][files]") {
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

REX_TEST_CASE("files: sequential reads advance the position", "[char][files]") {
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

REX_TEST_CASE("files: full attributes report the size", "[char][files]") {
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

REX_TEST_CASE("files: a creation disposition above overwrite if is an invalid parameter",
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

REX_TEST_CASE("files: XamContentCreate fails when the content header cannot be written",
              "[char][files][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kTitleId = 0x52455831;
    const u64 xuid = c.runtime().content_manager().user_profile().xuid();
    const auto header_path = c.runtime().user_data_root() / std::format("{:016X}", xuid) /
                             std::format("{:08X}", kTitleId) / "Headers" /
                             std::format("{:08X}", 1u) / "rexsave.header";
    std::filesystem::create_directories(header_path);

    using rex::runtime::XCONTENT_AGGREGATE_DATA;
    const u32 data = c.Alloc(sizeof(XCONTENT_AGGREGATE_DATA));
    auto* host = c.runtime().memory()->TranslateVirtual<XCONTENT_AGGREGATE_DATA*>(data);
    std::memset(static_cast<void*>(host), 0, sizeof(XCONTENT_AGGREGATE_DATA));
    host->device_id = 1;
    host->content_type = rex::runtime::XContentType::kSavedGame;
    std::memcpy(host->file_name_raw, "rexsave", 7);
    host->xuid = xuid;
    host->title_id = kTitleId;
    const u32 root = c.String("save");

    const auto package_path = c.runtime().user_data_root() / std::format("{:016X}", xuid) /
                              std::format("{:08X}", kTitleId) / std::format("{:08X}", 1u) /
                              "rexsave";
    REQUIRE_FALSE(std::filesystem::exists(package_path));

    CHECK(c.XAM("XamContentCreateInternal", {root, data, 1, 0, 0, 0, 0, 0}) != 0);
    CHECK_FALSE(c.runtime().content_manager().IsContentOpen(*host));
    CHECK_FALSE(std::filesystem::exists(package_path));
  });
}

REX_TEST_CASE("files: closing content with no link leaves other files open",
              "[char][files][content]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "k.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\k.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    c.runtime().content_manager().CloseOpenedFilesFromContent("nolink");
    CHECK(c.runtime().object_table().Get(handle));
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: closing content leaves a sibling root that shares its prefix open",
              "[char][files][content]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "conta");
    std::filesystem::create_directories(root / "contab");
    std::ofstream(root / "contab" / "k.bin", std::ios::binary) << "xxxx";
  });
  namespace fs = rex::filesystem;
  auto* file_system = fixture.api().Get<fs::IFileSystem>();
  REQUIRE(file_system);
  REQUIRE(file_system->Mount({"\\Device\\ContA", fs::ProviderKind::kHostDirectory,
                              fixture.root() / "conta", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Mount({"\\Device\\ContAB", fs::ProviderKind::kHostDirectory,
                              fixture.root() / "contab", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Link("conta:", "\\Device\\ContA"));
  REQUIRE(file_system->Link("contab:", "\\Device\\ContAB"));
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("contab:\\k.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    c.runtime().content_manager().CloseOpenedFilesFromContent("conta");
    CHECK(c.runtime().object_table().Get(handle));
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: an open with a root handle that does not resolve is an invalid handle",
              "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "h.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    const u32 attributes = c.Attributes("h.bin");
    c.Store32(attributes, 0x1234);
    CHECK(c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, attributes, status_block, 0, 0, 3,
                                    1, 0x20}) == kInvalidHandle);
    const u32 info = c.Alloc(56);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {attributes, info}) == kInvalidHandle);
  });
}

REX_TEST_CASE("files: full attributes resolve a name under a root directory", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "s.bin", 12); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("scratch:\\"),
                                           status_block, 0, 0, 3, 1, 0x1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 directory = c.Load32(handle_ptr);
    const u32 attributes = c.Attributes("s.bin");
    c.Store32(attributes, directory);
    const u32 info = c.Alloc(56);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {attributes, info}) == 0);
    CHECK(c.Load32(info + 44) == 12);
    CHECK(c.Kernel("NtClose", {directory}) == 0);
  });
}

REX_TEST_CASE("files: a symbolic link open ignores its root directory", "[char][files]") {
  KernelFixture fixture;
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 attributes = c.Attributes("scratch:");
    c.Store32(attributes, 0x1234);
    c.Store32(attributes + 8, 0x40);
    CHECK(c.Kernel("NtOpenSymbolicLinkObject", {handle_ptr, attributes}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
  });
}

REX_TEST_CASE("files: unsupported information classes keep their results", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "m.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\m.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    CHECK(c.Kernel("NtQueryInformationFile",
                   {handle, status_block, info, 8, rex::runtime::XFileModeInformation}) ==
          rex::X_STATUS_INVALID_PARAMETER);
    c.Store32(info, 0);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::runtime::XFileModeInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a seek moves from each origin and sets the length there", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "p.bin", 10); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    using rex::runtime::SeekOrigin;
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\p.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    {
      auto file = c.runtime().object_table().Get<rex::runtime::FileObject>(handle);
      REQUIRE(file);
      CHECK(file->Seek(4, SeekOrigin::kBegin) == 4);
      CHECK(file->Seek(3, SeekOrigin::kCurrent) == 7);
      CHECK(file->Seek(-2, SeekOrigin::kCurrent) == 5);
      CHECK(file->Seek(-1, SeekOrigin::kEnd) == 9);
      CHECK(file->position() == 9);
      CHECK(file->SetLengthToPosition() == rex::X_STATUS_SUCCESS);
    }
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.root() / "scratch" / "p.bin") == 9);
}

REX_TEST_CASE("files: a read queues its APC without a context", "[char][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "r.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.Attributes("scratch:\\r.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtReadFile", {handle, 0, routine, 0, status_block, buffer, 4, 0}) == 0);
    CHECK(DeliverAPCs(c) == rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_context == 0);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a failed write queues no APC", "[char][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "w.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("scratch:\\w.bin"),
                                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtWriteFile", {handle, 0, routine, 0x1234, status_block, buffer, 4, 0}) != 0);
    CHECK(DeliverAPCs(c) == rex::X_STATUS_TIMEOUT);
    CHECK(g_apc_calls == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a directory query signals its event and queues its APC",
              "[char][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "d.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("scratch:\\"),
                                           status_block, 0, 0, 3, 1, 0x1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 directory = c.Load32(handle_ptr);
    const u32 event = NewEvent(c);
    const u32 info = c.Alloc(0x100);
    CHECK(c.Kernel("NtQueryDirectoryFile",
                   {directory, event, routine, 0, status_block, info, 0x100, 0, 0}) == 0);
    CHECK(DeliverAPCs(c) == rex::X_STATUS_USER_APC);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.Quad(0)}) == 0);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {event}) == 0);
    CHECK(c.Kernel("NtClose", {directory}) == 0);
  });
}

REX_TEST_CASE(
    "files: a device control writes its status block, signals its event and queues its "
    "APC",
    "[char][files][apc]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 status_block = c.Alloc(8);
    c.Store32(status_block, 0xFFFFFFFF);
    c.Store32(status_block + 4, 0xFFFFFFFF);
    const u32 event = NewEvent(c);
    const u32 geometry = c.Alloc(8);
    CHECK(c.Kernel("NtDeviceIoControlFile",
                   {0, event, routine, 0, status_block, 0x70000, 0, 0, geometry, 8}) == 0);
    CHECK(c.Load32(status_block) == 0);
    CHECK(c.Load32(status_block + 4) == 8);
    CHECK(DeliverAPCs(c) == rex::X_STATUS_USER_APC);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.Quad(0)}) == 0);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {event}) == 0);
  });
}
