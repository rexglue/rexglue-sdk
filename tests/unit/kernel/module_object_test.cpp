/**
 * @file        tests/unit/kernel/module_object_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <rex/ppc/call.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/heap.h>

#include "base/xex_builder.h"
#include "kernel/kernel_fixture.h"
#include "kernel/xboxkrnl/loaded_command_line.h"
#include "pe/pe_image.h"
#include "kernel/module_registry.h"
#include "kernel/title_state.h"

namespace {

using rex::kernel::ModuleFormat;
using rex::kernel::ModuleObject;
using rex::kernel::ModuleType;

class CountingModule : public rex::kernel::GuestModule {
 public:
  CountingModule(rex::kernel::FunctionDispatcher* dispatcher, u32* destroyed)
      : GuestModule(dispatcher), destroyed_(destroyed) {}
  ~CountingModule() override { ++*destroyed_; }
  const std::string& GetName() const override { return name_; }
  bool IsExecutable() const override { return false; }

 private:
  std::string name_ = "counting";
  u32* destroyed_;
};

class WatchedXEXModule : public rex::kernel::XEXModule {
 public:
  WatchedXEXModule(rex::kernel::TitleRuntime& runtime, u32* destroyed, bool* loaded_at_destroy)
      : XEXModule(StateOf(runtime).GetFunctionDispatcher(), runtime),
        destroyed_(destroyed),
        loaded_at_destroy_(loaded_at_destroy) {}
  ~WatchedXEXModule() override {
    ++*destroyed_;
    *loaded_at_destroy_ = IsLoaded();
  }

 private:
  u32* destroyed_;
  bool* loaded_at_destroy_;
};

}  // namespace

REX_TEST_CASE("A user module object frees its loaded module when it is destroyed",
              "[kernel][module]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime& runtime = fixture.GetRuntime();
  u32 destroyed = 0;
  const rex::kernel::X_HANDLE handle = rex::kernel::CreateModule(runtime, ModuleType::kUser);
  REQUIRE(handle != 0);
  {
    auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::kELF;
    module->processor_module =
        std::make_unique<CountingModule>(StateOf(runtime).GetFunctionDispatcher(), &destroyed);
  }
  CHECK(destroyed == 0);
  rex::kernel::CloseOwned(runtime.GetObjectTable(), handle);
  CHECK(destroyed == 1);
}

REX_TEST_CASE("A user module object unloads its XEX image before freeing the module",
              "[kernel][module]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime& runtime = fixture.GetRuntime();
  rex::test::XEXSpec spec;
  spec.image = rex::test::TestImage(0x1000);
  const auto file = rex::test::BuildXEX(spec);
  u32 destroyed = 0;
  bool loaded_at_destroy = true;
  auto xex = std::make_unique<WatchedXEXModule>(runtime, &destroyed, &loaded_at_destroy);
  REQUIRE(xex->Load("watched", "game:\\watched.xex", file.data(), file.size()));
  rex::kernel::BaseHeap* heap = StateOf(runtime).GetAddressSpace()->LookupHeap(spec.load_address);
  rex::kernel::HeapAllocation info{};
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  REQUIRE(info.state != 0);

  const rex::kernel::X_HANDLE handle = rex::kernel::CreateModule(runtime, ModuleType::kUser);
  REQUIRE(handle != 0);
  {
    auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::kXEX;
    module->processor_module = std::move(xex);
  }
  rex::kernel::CloseOwned(runtime.GetObjectTable(), handle);
  CHECK(destroyed == 1);
  CHECK_FALSE(loaded_at_destroy);
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  CHECK(info.state == 0);
}

namespace {
void ExportedFunction(PPCContext& ctx, u8*) {
  ctx.r3.u64 = 0;
}

constexpr u32 kExportImageBase = 0x8F000000;
constexpr u32 kExportImageSize = 0x2000;
constexpr u32 kExportTableOffset = 0x1200;
constexpr u32 kExportFunctionOffset = 0x1100;
constexpr u32 kExportOrdinal = 1;

std::vector<u8> BuildExportImage() {
  std::vector<u8> image(kExportImageSize, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = 1;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = 0x1000;
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = 0x1000;
  section->Misc.VirtualSize = 0x1000;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  return image;
}
}  // namespace

REX_TEST_CASE("A user module hands the same thunk to a caller that asks for an ordinal again",
              "[kernel][module]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime& runtime = fixture.GetRuntime();
  rex::test::XEXSpec spec;
  spec.load_address = kExportImageBase;
  spec.image = BuildExportImage();
  rex::test::PutBE32(spec.image, kExportTableOffset + 0x20, kExportImageBase >> 16);
  rex::test::PutBE32(spec.image, kExportTableOffset + 0x24, 1);
  rex::test::PutBE32(spec.image, kExportTableOffset + 0x28, kExportOrdinal);
  rex::test::PutBE32(spec.image, kExportTableOffset + 0x2C, kExportFunctionOffset);
  spec.export_table = kExportImageBase + kExportTableOffset;
  const auto file = rex::test::BuildXEX(spec);
  auto xex =
      std::make_unique<rex::kernel::XEXModule>(StateOf(runtime).GetFunctionDispatcher(), runtime);
  REQUIRE(xex->Load("exporter", "game:\\exporter.xex", file.data(), file.size()));
  REQUIRE(xex->LoadContinue());
  auto* dispatcher = StateOf(runtime).GetFunctionDispatcher();
  REQUIRE(dispatcher->SetFunction(kExportImageBase + kExportFunctionOffset, &ExportedFunction));

  const rex::kernel::X_HANDLE handle = rex::kernel::CreateModule(runtime, ModuleType::kUser);
  REQUIRE(handle != 0);
  {
    auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::kXEX;
    module->processor_module = std::move(xex);

    const u32 caller = kExportImageBase + 0x104;
    const u32 first = module->GetExportAddress(runtime, kExportOrdinal, caller);
    CHECK(first != 0);
    CHECK(first != kExportImageBase + kExportFunctionOffset);
    CHECK(module->GetExportAddress(runtime, kExportOrdinal, caller) == first);

    module->InvalidateThunkCache(first, first + 4);
    const u32 renewed = module->GetExportAddress(runtime, kExportOrdinal, caller);
    CHECK(renewed != 0);
    CHECK(renewed != first);
    CHECK(module->GetExportAddress(runtime, kExportOrdinal, caller) == renewed);
  }
  rex::kernel::CloseOwned(runtime.GetObjectTable(), handle);
}

REX_TEST_CASE("The exception directory is read from the cached PE info when the header is zeroed",
              "[kernel][module]") {
  constexpr u32 kDirectoryRva = 0x1800;
  constexpr u32 kDirectorySize = 0x20;
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime& runtime = fixture.GetRuntime();
  rex::test::XEXSpec spec;
  spec.load_address = kExportImageBase;
  spec.image = BuildExportImage();
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(spec.image.data() + 0x80);
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = kDirectoryRva;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = kDirectorySize;
  const auto file = rex::test::BuildXEX(spec);
  rex::kernel::XEXModule xex(StateOf(runtime).GetFunctionDispatcher(), runtime);
  REQUIRE(xex.Load("exporter", "game:\\exporter.xex", file.data(), file.size()));
  REQUIRE(xex.LoadContinue());

  std::memset(StateOf(runtime).GetAddressSpace()->TranslateVirtual(kExportImageBase), 0, 0x1000);

  const rex::kernel::GuestModule& module = xex;
  CHECK(module.GetExceptionDirectoryRVA() == kDirectoryRva);
  CHECK(module.GetExceptionDirectorySize() == kDirectorySize);
  CHECK(module.GetExceptionDirectoryAddress() == kExportImageBase + kDirectoryRva);
}

REX_TEST_CASE("The loaded command line quotes the image name and appends the arguments",
              "[kernel][module]") {
  using rex::kernel::LoadedCommandLine;
  CHECK(LoadedCommandLine("game:\\title.xex", "") == "\"title.xex\"");
  CHECK(LoadedCommandLine("game:\\bin\\title.xex", "-level 2") == "\"title.xex\" -level 2");
  CHECK(LoadedCommandLine("game:\\default.xex", "") == "\"default.xex\"");
}

REX_TEST_CASE("The loaded command line fits the capacity for the longest image name",
              "[kernel][module]") {
  const std::string longest(kExLoadedImageNameSize - 1, 'a');
  CHECK(rex::kernel::LoadedCommandLine(longest, "-x").size() <
        rex::kernel::LoadedCommandLineCapacity("-x"));
}

namespace {

constexpr size_t kElfHeaderSize = 52;
constexpr size_t kElfProgramHeaderSize = 32;

void PutBE16(std::vector<u8>& bytes, size_t offset, u16 value) {
  bytes[offset] = static_cast<u8>(value >> 8);
  bytes[offset + 1] = static_cast<u8>(value);
}

void PutBE32(std::vector<u8>& bytes, size_t offset, u32 value) {
  PutBE16(bytes, offset, static_cast<u16>(value >> 16));
  PutBE16(bytes, offset + 2, static_cast<u16>(value));
}

std::vector<u8> MakeElf(u32 segment_type, u32 offset, u32 file_size, u32 align) {
  std::vector<u8> bytes(kElfHeaderSize + kElfProgramHeaderSize + 16);
  const u8 ident[] = {0x7F, 'E', 'L', 'F', 1, 2, 1};
  std::memcpy(bytes.data(), ident, sizeof(ident));
  PutBE16(bytes, 16, 2);
  PutBE16(bytes, 18, 20);
  PutBE32(bytes, 24, 0x82000000);
  PutBE32(bytes, 28, kElfHeaderSize);
  PutBE16(bytes, 40, kElfHeaderSize);
  PutBE16(bytes, 42, kElfProgramHeaderSize);
  PutBE16(bytes, 44, 1);
  PutBE32(bytes, kElfHeaderSize, segment_type);
  PutBE32(bytes, kElfHeaderSize + 4, offset);
  PutBE32(bytes, kElfHeaderSize + 8, 0x9F000000);
  PutBE32(bytes, kElfHeaderSize + 16, file_size);
  PutBE32(bytes, kElfHeaderSize + 20, file_size);
  PutBE32(bytes, kElfHeaderSize + 28, align);
  return bytes;
}

bool LoadElf(rex::test::KernelFixture& fixture, const std::vector<u8>& bytes) {
  rex::kernel::ELFModule module(StateOf(fixture.GetRuntime()).GetFunctionDispatcher());
  return module.Load("test.elf", "game:\\test.elf", bytes.data(), bytes.size());
}

}  // namespace

REX_TEST_CASE("An ELF with an unsupported header is refused instead of read out of bounds",
              "[kernel][module][elf]") {
  rex::test::KernelFixture fixture;
  CHECK(LoadElf(fixture, MakeElf(4, 0, 0, 0)));

  auto elf64 = MakeElf(4, 0, 0, 0);
  elf64[4] = 2;
  CHECK_FALSE(LoadElf(fixture, elf64));

  auto wide_entries = MakeElf(4, 0, 0, 0);
  PutBE16(wide_entries, 42, kElfProgramHeaderSize + 8);
  CHECK_FALSE(LoadElf(fixture, wide_entries));

  auto outside = MakeElf(4, 0, 0, 0);
  PutBE32(outside, 28, 0x1000);
  CHECK_FALSE(LoadElf(fixture, outside));

  auto many = MakeElf(4, 0, 0, 0);
  PutBE16(many, 44, 0xFFFF);
  CHECK_FALSE(LoadElf(fixture, many));

  auto truncated = MakeElf(4, 0, 0, 0);
  truncated.resize(kElfHeaderSize - 1);
  CHECK_FALSE(LoadElf(fixture, truncated));
}

REX_TEST_CASE("An ELF segment outside the file is refused", "[kernel][module][elf]") {
  rex::test::KernelFixture fixture;
  CHECK_FALSE(LoadElf(fixture, MakeElf(1, 0x10000, 0x100, 0x1000)));
  CHECK_FALSE(LoadElf(fixture, MakeElf(1, kElfHeaderSize, 0xFFFFFFF0, 0x1000)));
  CHECK_FALSE(LoadElf(fixture, MakeElf(1, 0, 0x10, 3)));
}

REX_TEST_CASE("A module that is not an XEX answers its XEX only calls with a failure",
              "[kernel][module]") {
  rex::test::KernelFixture fixture;
  ModuleObject module(&fixture.GetRuntime(), ModuleType::kUser);
  module.user.module_format = ModuleFormat::kELF;
  CHECK(module.GetXEX() == nullptr);
  CHECK(module.GetExportAddress("Export") == 0);
  CHECK(module.GetExportAddress(fixture.GetRuntime(), 1, 0) == 0);
  u32 section_data = 0;
  u32 section_size = 0;
  CHECK(module.GetSection("section", &section_data, &section_size) ==
        rex::kernel::X_STATUS_NOT_FOUND);
  void* header = nullptr;
  CHECK(module.GetOptHeader(rex::kernel::kXEXHeaderExecutionInfo, &header) ==
        rex::kernel::X_STATUS_UNSUCCESSFUL);

  ModuleObject undefined(&fixture.GetRuntime(), ModuleType::kUser);
  CHECK(undefined.GetXEX() == nullptr);
  CHECK(undefined.GetExportAddress("Export") == 0);
}
