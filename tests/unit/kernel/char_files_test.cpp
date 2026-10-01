#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include <rex/system/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = X_STATUS_INVALID_HANDLE;
}  // namespace

TEST_CASE("files: create read write seek and query", "[char][files]") {
  KernelFixture fixture([](const std::filesystem::path& root) {
    std::ofstream out(root / "data.bin", std::ios::binary);
    const char bytes[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    out.write(bytes, sizeof(bytes));
  });
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    REQUIRE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\data.bin"),
                                      status_block, 0, 0, 3, 1, 0x20}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    const u32 buffer = c.Alloc(16);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(0)}) == 0);
    CHECK(c.Load32(status_block + 4) == 8);
    CHECK(c.Load32(buffer) == 0x00010203);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(8)}) == 0);
    CHECK(c.Load32(buffer) == 0x08090A0B);
    CHECK(c.Kernel("NtReadFile", {handle, 0, 0, 0, status_block, buffer, 8, c.Quad(16)}) ==
          X_STATUS_END_OF_FILE);
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
                                    status_block, 0, 0, 3, 1, 0x20}) == X_STATUS_NO_SUCH_FILE);
  });
}

TEST_CASE("io completion: set remove and timeout", "[char][files]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 port_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateIoCompletion", {port_ptr, 0x1F0003, 0, 1}) == 0);
    const u32 port = c.Load32(port_ptr);
    const u32 key = c.Alloc(4);
    const u32 context = c.Alloc(4);
    const u32 status_block = c.Alloc(8);
    CHECK(c.Kernel("NtRemoveIoCompletion", {port, key, context, status_block, c.Quad(0)}) ==
          X_STATUS_TIMEOUT);
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
    REQUIRE(c.Kernel("NtCreateFile", {handle_ptr, 0x80100000, c.Attributes("game:\\seq.bin"),
                                      status_block, 0, 0, 3, 1, 0x20}) == 0);
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
