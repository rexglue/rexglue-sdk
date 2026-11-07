/**
 * @file        tests/unit/kernel/files_test.cpp
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

#include <rex/filesystem/mount.h>
#include <rex/kernel/types/content.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/io.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/module.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"
#include "xam/xam_state.h"

using rex::kernel::X_STATUS;

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = rex::kernel::X_STATUS_INVALID_HANDLE;
constexpr u32 kReadWriteAccess = 0xC0100000;
constexpr u16 kOrdinalIoCompletionType = 0x0036;
constexpr u16 kOrdinalIoFileType = 0x003E;

u32 ExportedType(Caller& c, u16 ordinal) {
  auto& runtime = c.GetRuntime();
  const auto kernel_module = runtime.GetModuleRegistry().FindKernelModule("xboxkrnl.exe");
  REQUIRE(kernel_module);
  auto module = runtime.GetObjectTable().Get<rex::kernel::ModuleObject>(*kernel_module);
  REQUIRE(module);
  return module->GetVariable(ordinal);
}

u32 HeaderType(Caller& c, u32 body) {
  auto* memory = StateOf(c.GetRuntime()).GetAddressSpace();
  return *memory->TranslateVirtual<const be_u32*>(body - 8);
}

bool ObjectAlive(Caller& c, u32 address) {
  return static_cast<bool>(c.GetRuntime().GetObjectTable().GetByAddress(address));
}

void MountScratch(KernelFixture& fixture) {
  namespace fs = rex::filesystem;
  auto* file_system = StateOf(fixture.GetRuntime()).GetFileSystem();
  REQUIRE(file_system);
  REQUIRE(file_system->Mount({"\\Device\\Scratch", fs::ProviderType::kHostDirectory,
                              fixture.GetRoot() / "scratch", fs::MountFlags::kWritable}));
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
  return StateOf(c.GetRuntime())
      .GetFunctionDispatcher()
      ->AllocateThunk(
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
  const u32 result = c.Kernel("NtWaitForSingleObjectEx", {event, 1, 1, c.AllocQuad(-10000)});
  c.Kernel("NtClose", {event});
  return result;
}
}  // namespace

REX_TEST_CASE("files: an allocation size only truncates below the end of file", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "a.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\a.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    c.Store64(info, 4096);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::kernel::XFileAllocationInformation}) == 0);
    c.Store64(info, 8);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::kernel::XFileAllocationInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.GetRoot() / "scratch" / "a.bin") == 8);
}

REX_TEST_CASE("files: a flush reaches the file and fails on a bad handle", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "f.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\f.bin"),
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

REX_TEST_CASE("files: a rename onto a file needs replace existing", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    WriteScratch(root, "r1.bin", 16);
    WriteScratch(root, "r2.bin", 4);
  });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\r1.bin"),
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
    c.Store32(info + 12, c.AllocString(kTarget));
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 16, rex::kernel::XFileRenameInformation}) ==
          rex::kernel::X_STATUS_OBJECT_NAME_COLLISION);
    c.Store32(info, 1);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 16, rex::kernel::XFileRenameInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK_FALSE(std::filesystem::exists(fixture.GetRoot() / "scratch" / "r1.bin"));
  CHECK(std::filesystem::file_size(fixture.GetRoot() / "scratch" / "r2.bin") == 16);
}

REX_TEST_CASE("files: create accepts ; and = and rename rejects them", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "n.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    for (const char* name : {"scratch:\\a;b.bin", "scratch:\\a=b.bin"}) {
      CHECK(c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.AllocAttributes(name),
                                      status_block, 0, 0, 3, 2, 0x20}) == 0);
      CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
    }
    CHECK(c.Kernel("NtCreateFile",
                   {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\a+b.bin"),
                    status_block, 0, 0, 3, 2, 0x20}) == rex::kernel::X_STATUS_OBJECT_NAME_INVALID);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\n.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(16);
    for (const std::string_view target :
         {"scratch:\\a;b.bin", "scratch:\\a=b.bin", "scratch:\\a+b.bin"}) {
      c.Store32(info, 1);
      c.Store32(info + 4, 0);
      c.Store32(info + 8, static_cast<u32>(target.size() << 16 | target.size()));
      c.Store32(info + 12, c.AllocString(target));
      CHECK(c.Kernel("NtSetInformationFile",
                     {handle, status_block, info, 16, rex::kernel::XFileRenameInformation}) ==
            rex::kernel::X_STATUS_OBJECT_NAME_INVALID);
    }
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: an allocation size above the end of file keeps the size", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "b.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\b.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    c.Store64(info, 4096);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 8, rex::kernel::XFileAllocationInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.GetRoot() / "scratch" / "b.bin") == 16);
}

REX_TEST_CASE("files: create, positioned read, end of file and closed handle", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "data.bin", std::ios::binary);
    const char bytes[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("game:\\data.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(16);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.AllocQuad(0)}) == 0);
    CHECK(c.Load32(status_block + 4) == 8);
    CHECK(c.Load32(buffer) == 0x00010203);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.AllocQuad(8)}) == 0);
    CHECK(c.Load32(buffer) == 0x08090A0B);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.AllocQuad(16)}) ==
          rex::kernel::X_STATUS_END_OF_FILE);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.AllocQuad(0)}) ==
          kInvalidHandle);
  });
}

REX_TEST_CASE("files: missing file", "[kernel][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECK(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("game:\\missing.bin"),
                                    status_block, 0, 0, 3, 1, 0x20}) ==
          rex::kernel::X_STATUS_NO_SUCH_FILE);
  });
}

REX_TEST_CASE("io completion: set remove and timeout", "[kernel][files]") {
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
    CHECK(c.Kernel("NtRemoveIoCompletion", {port, key, context, status_block, c.AllocQuad(0)}) ==
          rex::kernel::X_STATUS_TIMEOUT);
    CHECK(c.Kernel("NtSetIoCompletion", {port, 11, 22, 0, 33}) == 0);
    CHECK(c.Kernel("NtRemoveIoCompletion", {port, key, context, status_block, c.AllocQuad(0)}) ==
          0);
    CHECK(c.Load32(key) == 11);
    CHECK(c.Load32(context) == 22);
    CHECK(c.Load32(status_block + 4) == 33);
    CHECK(c.Kernel("NtClose", {port}) == 0);
  });
}

REX_TEST_CASE("files: a file object has a guest body that ObDereferenceObject destroys",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "o.bin", 16); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    const u32 object_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\o.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 file_type = ExportedType(c, kOrdinalIoFileType);
    REQUIRE(file_type != 0);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, file_type, object_ptr}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 body = c.Load32(object_ptr);
    CHECK(body != 0);
    CHECK(HeaderType(c, body) == file_type);
    auto* memory = StateOf(c.GetRuntime()).GetAddressSpace();
    CHECK(static_cast<u16>(*memory->TranslateVirtual<const be_u16*>(body)) ==
          rex::kernel::kFileObjectTypeId);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(ObjectAlive(c, body));
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK_FALSE(ObjectAlive(c, body));
  });
}

REX_TEST_CASE(
    "io completion: an IO completion object has a guest body that ObDereferenceObject "
    "destroys",
    "[kernel][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 port_ptr = c.Alloc(4);
    const u32 object_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateIoCompletion", {port_ptr, 0x1F0003, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 port = c.Load32(port_ptr);
    const u32 port_type = ExportedType(c, kOrdinalIoCompletionType);
    REQUIRE(port_type != 0);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {port, port_type, object_ptr}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 body = c.Load32(object_ptr);
    CHECK(body != 0);
    CHECK(HeaderType(c, body) == port_type);
    CHECK(c.Kernel("NtClose", {port}) == 0);
    CHECK(ObjectAlive(c, body));
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK_FALSE(ObjectAlive(c, body));
  });
}

REX_TEST_CASE("files: NtCreateIoCompletion initializes the queue header of its body",
              "[kernel][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const u32 concurrent : {0u, 3u}) {
      const u32 port_ptr = c.Alloc(4);
      const u32 object_ptr = c.Alloc(4);
      CHECKED_ELSE(c.Kernel("NtCreateIoCompletion", {port_ptr, 0x1F0003, 0, concurrent}) == 0) {
        FAIL_CHECK("the check above failed");
        return;
      }
      CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {c.Load32(port_ptr), 0, object_ptr}) ==
                   0) {
        FAIL_CHECK("the check above failed");
        return;
      }
      const u32 body = c.Load32(object_ptr);
      REQUIRE(body != 0);
      CHECK(c.Load8(body) == 4);
      CHECK(c.Load32(body + 4) == 0);
      CHECK(c.Load32(body + 0x8) == body + 0x8);
      CHECK(c.Load32(body + 0xC) == body + 0x8);
      CHECK(c.Load32(body + 0x10) == body + 0x10);
      CHECK(c.Load32(body + 0x14) == body + 0x10);
      CHECK(c.Load32(body + 0x18) == 0);
      CHECK(c.Load32(body + 0x1C) == (concurrent ? concurrent : 1));
      CHECK(c.Load32(body + 0x20) == body + 0x20);
      CHECK(c.Load32(body + 0x24) == body + 0x20);
    }
  });
}

REX_TEST_CASE("files: sequential reads advance the position", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "seq.bin", std::ios::binary);
    const char bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("game:\\seq.bin"),
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

REX_TEST_CASE("files: full attributes report the size", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "size.bin", std::ios::binary);
    const char bytes[16] = {};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 info = c.Alloc(0x38);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {c.AllocAttributes("game:\\size.bin"), info}) == 0);
    CHECK(c.Load32(info + 0x2C) == 16);
  });
}

REX_TEST_CASE("files: a creation disposition above overwrite if is an invalid parameter",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "d.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    for (const u32 disposition : {6u, 0x101u}) {
      CHECK(c.Kernel("NtCreateFile",
                     {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\d.bin"),
                      status_block, 0, 0, 3, disposition, 0x20}) ==
            rex::kernel::X_STATUS_INVALID_PARAMETER);
    }
  });
}

REX_TEST_CASE("files: XamContentCreate fails when the content header cannot be written",
              "[kernel][files][content]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    constexpr u32 kTitleId = 0x52455831;
    const u64 xuid = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
    const auto header_path = fixture.GetRoot() / std::format("{:016X}", xuid) /
                             std::format("{:08X}", kTitleId) / "Headers" /
                             std::format("{:08X}", 1u) / "rexsave.header";
    std::filesystem::create_directories(header_path);

    using rex::kernel::XCONTENT_AGGREGATE_DATA;
    const u32 data = c.Alloc(sizeof(XCONTENT_AGGREGATE_DATA));
    auto* host =
        StateOf(c.GetRuntime()).GetAddressSpace()->TranslateVirtual<XCONTENT_AGGREGATE_DATA*>(data);
    std::memset(static_cast<void*>(host), 0, sizeof(XCONTENT_AGGREGATE_DATA));
    host->device_id = 1;
    host->content_type = rex::kernel::XContentType::kSavedGame;
    std::memcpy(host->file_name_raw, "rexsave", 7);
    host->xuid = xuid;
    host->title_id = kTitleId;
    const u32 root = c.AllocString("save");

    const auto package_path = fixture.GetRoot() / std::format("{:016X}", xuid) /
                              std::format("{:08X}", kTitleId) / std::format("{:08X}", 1u) /
                              "rexsave";
    REQUIRE_FALSE(std::filesystem::exists(package_path));

    CHECK(c.XAM("XamContentCreateInternal", {root, data, 1, 0, 0, 0, 0, 0}) != 0);
    CHECK_FALSE(c.GetRuntime().GetContentManager().IsContentOpen(*host));
    CHECK_FALSE(std::filesystem::exists(package_path));
  });
}

REX_TEST_CASE("files: content is found under folders whose names differ in case",
              "[kernel][files][content][case]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&fixture](Caller& c) {
    constexpr u32 kTitleId = 0xABCDEF01;
    constexpr u64 kXUID = 0xABCDEF0123456789;
    const auto title_dir = fixture.GetRoot() / "abcdef0123456789" / "abcdef01";
    std::filesystem::create_directories(title_dir / "00000001" / "ReXSave");
    std::filesystem::create_directories(title_dir / "headers" / "00000001");

    rex::kernel::XCONTENT_AGGREGATE_DATA data{};
    data.device_id = 1;
    data.content_type = rex::kernel::XContentType::kSavedGame;
    std::memcpy(data.file_name_raw, "rexsave", 7);
    data.xuid = kXUID;
    data.title_id = kTitleId;
    CHECK(c.GetRuntime().GetContentManager().HasContent(kXUID, data));
  });
}

REX_TEST_CASE("files: deleting a symbolic link names it as creating it does",
              "[kernel][files][case]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const auto ansi = [&c](std::string_view text) {
      const u32 address = c.Alloc(8);
      c.Store32(address, (static_cast<u32>(text.size()) << 16) | static_cast<u32>(text.size() + 1));
      c.Store32(address + 4, c.AllocString(text));
      return address;
    };
    const u32 target = ansi("\\Device\\Harddisk0\\Partition1");
    CHECK(c.Kernel("ObCreateSymbolicLink", {ansi("\\??\\Linked:"), target}) == 0);
    CHECK(c.Kernel("ObDeleteSymbolicLink", {ansi("\\??\\LINKED:")}) == 0);
    CHECK(c.Kernel("ObDeleteSymbolicLink", {ansi("\\??\\Linked:")}) != 0);
  });
}

REX_TEST_CASE("files: closing content with no link leaves other files open",
              "[kernel][files][content]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "k.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\k.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    c.GetRuntime().GetContentManager().CloseOpenedFilesFromContent("nolink");
    CHECK(c.GetRuntime().GetObjectTable().Get(handle));
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: closing content leaves a sibling root that shares its prefix open",
              "[kernel][files][content]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "conta");
    std::filesystem::create_directories(root / "contab");
    std::ofstream(root / "contab" / "k.bin", std::ios::binary) << "xxxx";
  });
  namespace fs = rex::filesystem;
  auto* file_system = StateOf(fixture.GetRuntime()).GetFileSystem();
  REQUIRE(file_system);
  REQUIRE(file_system->Mount({"\\Device\\ContA", fs::ProviderType::kHostDirectory,
                              fixture.GetRoot() / "conta", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Mount({"\\Device\\ContAB", fs::ProviderType::kHostDirectory,
                              fixture.GetRoot() / "contab", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Link("conta:", "\\Device\\ContA"));
  REQUIRE(file_system->Link("contab:", "\\Device\\ContAB"));
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.AllocAttributes("contab:\\k.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    c.GetRuntime().GetContentManager().CloseOpenedFilesFromContent("conta");
    CHECK(c.GetRuntime().GetObjectTable().Get(handle));
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE(
    "files: closing content closes a file when the link target is cased differently than the mount",
    "[kernel][files][content][case]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "conta");
    std::ofstream(root / "conta" / "k.bin", std::ios::binary) << "xxxx";
  });
  namespace fs = rex::filesystem;
  auto* file_system = StateOf(fixture.GetRuntime()).GetFileSystem();
  REQUIRE(file_system);
  REQUIRE(file_system->Mount({"\\Device\\ContA", fs::ProviderType::kHostDirectory,
                              fixture.GetRoot() / "conta", fs::MountFlags::kWritable}));
  REQUIRE(file_system->Link("conta:", "\\DEVICE\\CONTA"));
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.AllocAttributes("conta:\\k.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    c.GetRuntime().GetContentManager().CloseOpenedFilesFromContent("conta");
    CHECK_FALSE(c.GetRuntime().GetObjectTable().Get(handle));
  });
}

REX_TEST_CASE("files: an open with a root handle that does not resolve is an invalid handle",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "h.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    const u32 attributes = c.AllocAttributes("h.bin");
    c.Store32(attributes, 0x1234);
    CHECK(c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, attributes, status_block, 0, 0, 3,
                                    1, 0x20}) == kInvalidHandle);
    const u32 info = c.Alloc(56);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {attributes, info}) == kInvalidHandle);
  });
}

REX_TEST_CASE("files: full attributes resolve a name under a root directory", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "s.bin", 12); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("scratch:\\"),
                                           status_block, 0, 0, 3, 1, 0x1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 directory = c.Load32(handle_ptr);
    const u32 attributes = c.AllocAttributes("s.bin");
    c.Store32(attributes, directory);
    const u32 info = c.Alloc(56);
    CHECK(c.Kernel("NtQueryFullAttributesFile", {attributes, info}) == 0);
    CHECK(c.Load32(info + 44) == 12);
    CHECK(c.Kernel("NtClose", {directory}) == 0);
  });
}

REX_TEST_CASE("files: a symbolic link open ignores its root directory", "[kernel][files]") {
  KernelFixture fixture;
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 attributes = c.AllocAttributes("scratch:");
    c.Store32(attributes, 0x1234);
    c.Store32(attributes + 8, 0x40);
    CHECK(c.Kernel("NtOpenSymbolicLinkObject", {handle_ptr, attributes}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
  });
}

REX_TEST_CASE("files: a symbolic link open takes any attributes", "[kernel][files]") {
  KernelFixture fixture;
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    for (const u32 flags : {0u, 0x240u}) {
      const u32 attributes = c.AllocAttributes("scratch:");
      c.Store32(attributes + 8, flags);
      CHECK(c.Kernel("NtOpenSymbolicLinkObject", {handle_ptr, attributes}) == 0);
      CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
    }
  });
}

REX_TEST_CASE("files: a file path with the device prefix resolves like the bare path",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "s.bin", 12); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("\\??\\scratch:\\s.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
    const u32 info = c.Alloc(56);
    CHECK(c.Kernel("NtQueryFullAttributesFile",
                   {c.AllocAttributes("\\??\\scratch:\\s.bin"), info}) == 0);
    CHECK(c.Load32(info + 44) == 12);
    const u32 link = c.AllocAttributes("\\??\\scratch:");
    c.Store32(link + 8, 0x40);
    CHECK(c.Kernel("NtOpenSymbolicLinkObject", {handle_ptr, link}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
  });
}

REX_TEST_CASE("files: an invalid information class is refused and the mode class checks its length",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "m.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\m.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 info = c.Alloc(8);
    CHECK(c.Kernel("NtQueryInformationFile", {handle, status_block, info, 8, 0x7F}) ==
          rex::kernel::X_STATUS_INVALID_INFO_CLASS);
    c.Store32(info, 0);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 3, rex::kernel::XFileModeInformation}) ==
          rex::kernel::X_STATUS_INFO_LENGTH_MISMATCH);
    CHECK(c.Kernel("NtSetInformationFile",
                   {handle, status_block, info, 4, rex::kernel::XFileModeInformation}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a seek moves from each origin and sets the length there", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "p.bin", 10); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    using rex::kernel::SeekOrigin;
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\p.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    {
      auto file = c.GetRuntime().GetObjectTable().Get<rex::kernel::FileObject>(handle);
      REQUIRE(file);
      CHECK(file->Seek(4, SeekOrigin::kBegin) == 4);
      CHECK(file->Seek(3, SeekOrigin::kCurrent) == 7);
      CHECK(file->Seek(-2, SeekOrigin::kCurrent) == 5);
      CHECK(file->Seek(-1, SeekOrigin::kEnd) == 9);
      CHECK(file->GetPosition() == 9);
      CHECK(file->SetLengthToPosition() == rex::kernel::X_STATUS_SUCCESS);
    }
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
  CHECK(std::filesystem::file_size(fixture.GetRoot() / "scratch" / "p.bin") == 9);
}

REX_TEST_CASE("files: a read queues its APC without a context", "[kernel][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "r.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile",
                          {handle_ptr, kReadWriteAccess, c.AllocAttributes("scratch:\\r.bin"),
                           status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtReadFile", {handle, 0, routine, 0, status_block, buffer, 4, 0}) == 0);
    CHECK(DeliverAPCs(c) == rex::kernel::X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_context == 0);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a failed write queues no APC", "[kernel][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "w.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("scratch:\\w.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtWriteFile", {handle, 0, routine, 0x1234, status_block, buffer, 4, 0}) != 0);
    CHECK(DeliverAPCs(c) == rex::kernel::X_STATUS_TIMEOUT);
    CHECK(g_apc_calls == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a directory query signals its event and queues its APC",
              "[kernel][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "d.bin", 4); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("scratch:\\"),
                                           status_block, 0, 0, 3, 1, 0x1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 directory = c.Load32(handle_ptr);
    const u32 event = NewEvent(c);
    const u32 info = c.Alloc(0x100);
    CHECK(c.Kernel("NtQueryDirectoryFile",
                   {directory, event, routine, 0, status_block, info, 0x100, 0, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.AllocQuad(0)}) == 0);
    CHECK(DeliverAPCs(c) == rex::kernel::X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {event}) == 0);
    CHECK(c.Kernel("NtClose", {directory}) == 0);
  });
}

REX_TEST_CASE(
    "files: a device control writes its status block, signals its event and queues its "
    "APC",
    "[kernel][files][apc]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "data.bin", std::ios::binary);
    out.write("0123456789ABCDEF", 16);
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = RecordingAPC(c);
    const u32 status_block = c.Alloc(8);
    c.Store32(status_block, 0xFFFFFFFF);
    c.Store32(status_block + 4, 0xFFFFFFFF);
    const u32 event = NewEvent(c);
    const u32 geometry = c.Alloc(8);
    CHECK(c.Kernel("NtDeviceIoControlFile", {0, event, routine, 0, status_block, 0x70000, 0, 0,
                                             geometry, 8}) == kInvalidHandle);
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(
        c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.AllocAttributes("game:\\data.bin"),
                                  status_block, 0, 0, 3, 1, 0x20}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 device = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtDeviceIoControlFile",
                   {device, event, routine, 0, status_block, 0x70000, 0, 0, geometry, 8}) == 0);
    CHECK(c.Load32(status_block) == 0);
    CHECK(c.Load32(status_block + 4) == 8);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.AllocQuad(0)}) == 0);
    CHECK(DeliverAPCs(c) == rex::kernel::X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    CHECK(g_apc_status_block == status_block);
    CHECK(c.Kernel("NtClose", {event}) == 0);
    CHECK(c.Kernel("NtClose", {device}) == 0);
  });
}

namespace {
constexpr u32 kPrefill = 0xA5A5A5A5;

u32 OpenScratch(Caller& c, const char* path, u32 create_options) {
  const u32 handle_ptr = c.Alloc(4);
  const u32 status_block = c.Alloc(8);
  CHECKED_ELSE(c.Kernel("NtCreateFile", {handle_ptr, kReadWriteAccess, c.AllocAttributes(path),
                                         status_block, 0, 0, 3, 1, create_options}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(handle_ptr);
}

u32 PrefilledBlock(Caller& c) {
  const u32 block = c.Alloc(8);
  c.Store32(block, kPrefill);
  c.Store32(block + 4, kPrefill);
  return block;
}
}  // namespace

REX_TEST_CASE("files: an async read returns its status and completes", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "a.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = OpenScratch(c, "scratch:\\a.bin", 0);
    const u32 event = NewEvent(c);
    const u32 block = PrefilledBlock(c);
    const u32 buffer = c.Alloc(8);
    CHECK(c.Kernel("NtReadFile", {handle, event, 0, 0, block, buffer, 8, c.AllocQuad(0)}) == 0);
    CHECK(c.Load32(block) == 0);
    CHECK(c.Load32(block + 4) == 8);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.AllocQuad(0)}) == 0);
    CHECK(c.Kernel("NtClose", {event}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: an async read at end of file leaves its block and event alone",
              "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "e.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = OpenScratch(c, "scratch:\\e.bin", 0);
    const u32 event = NewEvent(c);
    const u32 block = PrefilledBlock(c);
    const u32 buffer = c.Alloc(8);
    CHECK(c.Kernel("NtReadFile", {handle, event, 0, 0, block, buffer, 8, c.AllocQuad(8)}) ==
          rex::kernel::X_STATUS_END_OF_FILE);
    CHECK(c.Load32(block) == kPrefill);
    CHECK(c.Load32(block + 4) == kPrefill);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.AllocQuad(0)}) ==
          rex::kernel::X_STATUS_TIMEOUT);
    CHECK(c.Kernel("NtClose", {event}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: an async write returns its status", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "s.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = OpenScratch(c, "scratch:\\s.bin", 0);
    const u32 block = PrefilledBlock(c);
    const u32 buffer = c.Alloc(4);
    CHECK(c.Kernel("NtWriteFile", {handle, 0, 0, 0, block, buffer, 4, c.AllocQuad(0)}) == 0);
    CHECK(c.Load32(block) == 0);
    CHECK(c.Load32(block + 4) == 4);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("files: a synchronous read error leaves its block alone", "[kernel][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) { WriteScratch(root, "y.bin", 8); });
  MountScratch(fixture);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = OpenScratch(c, "scratch:\\y.bin", 0x20);
    const u32 block = PrefilledBlock(c);
    const u32 buffer = c.Alloc(8);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, block, buffer, 8, c.AllocQuad(8)}) ==
          rex::kernel::X_STATUS_END_OF_FILE);
    CHECK(c.Load32(block) == kPrefill);
    CHECK(c.Load32(block + 4) == kPrefill);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}
