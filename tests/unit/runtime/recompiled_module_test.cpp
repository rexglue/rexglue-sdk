/**
 * @file        tests/unit/runtime/recompiled_module_test.cpp
 * @brief       Loading, unloading and reloading user modules that have a recompiled library
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>

#include "core/xex_builder.h"
#include "pe/pe_image.h"
#include "kernel/kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u32 kImageBase = 0x8E000000;
constexpr u32 kFunctionAddress = kImageBase + 0x100;
constexpr u32 kResultA = 0xA1;
constexpr u32 kResultB = 0xB2;
constexpr const char* kPathA = "game:\\a.xex";
constexpr const char* kPathB = "game:\\b.xex";

constexpr u32 kImageSize = 0x10000;
constexpr u32 kHeaderSize = 0x1000;
constexpr u32 kHeaderPadding = 0x400;
constexpr u32 kMarkerOffset = kHeaderSize + 0x10;
constexpr u32 kMarkerSize = 0x20;
constexpr u32 kPatchCopyOffset = kHeaderSize + 0x100;

std::vector<u8> BuildPEImage() {
  std::vector<u8> image(kImageSize, 0);
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
  section->Misc.VirtualSize = 0x200;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  std::memset(image.data() + kHeaderPadding, 0xAB, 0x40);
  std::memset(image.data() + kMarkerOffset, 0xCD, kMarkerSize);
  return image;
}

std::vector<u8> BuildImageFile() {
  rex::test::XEXSpec spec;
  spec.load_address = kImageBase;
  spec.page_size_4kb = false;
  spec.image = BuildPEImage();
  return rex::test::BuildXEX(spec);
}

void WritePatch(const std::filesystem::path& root, const char* name) {
  const auto base_file = BuildImageFile();
  auto base = rex::filesystem::XEXImage::Parse(base_file);
  REQUIRE(base);
  rex::test::XEXSpec spec;
  spec.module_flags = rex::filesystem::kXEXModulePatchDelta;
  spec.compression = rex::filesystem::kXEXCompressionDelta;
  spec.patch_descriptor = rex::test::BuildPatchDescriptor(*base);
  spec.patch_body = rex::test::BuildPatchBody(kPatchCopyOffset, kMarkerOffset);
  const auto file = rex::test::BuildXEX(spec);
  std::ofstream out(root / name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void WriteImage(const std::filesystem::path& root, const char* name) {
  const auto file = BuildImageFile();
  std::ofstream out(root / name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void PopulateImages(const std::filesystem::path& root) {
  WriteImage(root, "a.xex");
  WriteImage(root, "b.xex");
}

void PopulatePatched(const std::filesystem::path& root) {
  PopulateImages(root);
  WritePatch(root, "a.xexp");
}

void RegisterImages(rex::Runtime& runtime) {
  auto& modules = runtime.GetModuleRegistry();
  modules.RegisterRecompiledModule("a", kPathA, REX_TEST_RECOMPILED_A_NAME);
  modules.RegisterRecompiledModule("b", kPathB, REX_TEST_RECOMPILED_B_NAME);
}

u32 CallFunction(rex::Runtime& runtime) {
  u64 args[1] = {0};
  return static_cast<u32>(runtime.GetFunctionDispatcher()->Execute(rex::runtime::ThreadState::Get(),
                                                                   kFunctionAddress, args, 0));
}
}  // namespace

REX_TEST_CASE("modules: a recompiled module loads again after it was unloaded",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto first = modules.LoadUserModule(kPathA);
    REQUIRE(first);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*first);
    auto second = modules.LoadUserModule(kPathA);
    REQUIRE(second);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*second);
  });
}

REX_TEST_CASE("modules: a recompiled module loads at a base another one released",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto first = modules.LoadUserModule(kPathA);
    REQUIRE(first);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*first);
    auto second = modules.LoadUserModule(kPathB);
    REQUIRE(second);
    CHECK(CallFunction(runtime) == kResultB);
    modules.UnloadUserModule(*second);
    auto third = modules.LoadUserModule(kPathA);
    REQUIRE(third);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*third);
  });
}

REX_TEST_CASE("modules: a second recompiled module at a held base is refused and the first stays",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto first = modules.LoadUserModule(kPathA);
    REQUIRE(first);
    CHECK_FALSE(modules.LoadUserModule(kPathB));

    rex::runtime::HeapAllocationInfo info{};
    REQUIRE(runtime.GetMemory()->LookupHeap(kImageBase)->QueryRegionInfo(kImageBase, &info));
    CHECK(info.state != 0);
    CHECK(info.allocation_base == kImageBase);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*first);

    auto again = modules.LoadUserModule(kPathB);
    REQUIRE(again);
    CHECK(CallFunction(runtime) == kResultB);
    modules.UnloadUserModule(*again);
  });
}

REX_TEST_CASE("modules: XexUnloadImageAndExitThread unloads the module and ends the thread",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    const u32 hmodule_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("XexLoadImage", {c.String(kPathA), 8, 0, hmodule_ptr}) == 0);
    const u32 hmodule = c.Load32(hmodule_ptr);
    REQUIRE(hmodule != 0);

    const u32 handle =
        c.Spawn([hmodule](Caller& o) { o.Kernel("XexUnloadImageAndExitThread", {hmodule, 7}); });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);

    CHECK_FALSE(modules.GetModule(kPathA));
    auto second = modules.LoadUserModule(kPathB);
    REQUIRE(second);
    CHECK(CallFunction(runtime) == kResultB);
    modules.UnloadUserModule(*second);
  });
}

REX_TEST_CASE("modules: the PE header pages read as zero once the module is loaded",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto first = modules.LoadUserModule(kPathA);
    REQUIRE(first);
    auto module = runtime.GetObjectTable().Get<rex::runtime::ModuleObject>(*first);
    REQUIRE(module);
    const auto& pe = module->GetXEX()->GetPe();
    CHECK(pe.header_size == kHeaderSize);
    REQUIRE(pe.sections.size() == 1);
    CHECK(std::string(pe.sections[0].name) == ".text");

    const u8* header = runtime.GetMemory()->TranslateVirtual(kImageBase);
    CHECK(std::all_of(header, header + kHeaderSize, [](u8 byte) { return byte == 0; }));
    CHECK(c.Kernel("RtlImageNtHeader", {kImageBase}) == 0);
    CHECK(CallFunction(runtime) == kResultA);
    module = {};
    modules.UnloadUserModule(*first);
  });
}

REX_TEST_CASE("modules: the PE header stays in host memory until the sections are copied",
              "[runtime][module]") {
  KernelFixture fixture(PopulateImages);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    const auto file = BuildImageFile();
    const rex::X_HANDLE created = rex::runtime::CreateUserModule(runtime);
    REQUIRE(created);
    {
      auto module = runtime.GetObjectTable().Get<rex::runtime::ModuleObject>(created);
      REQUIRE(module);
      REQUIRE(module->LoadFromMemory(runtime, file.data(), file.size()) == rex::X_STATUS_PENDING);
      auto* xex = module->GetXEX();

      const auto pending = xex->GetPendingImage();
      REQUIRE(pending.size() >= kImageSize);
      CHECK(pending[0] == 'M');
      CHECK(pending[1] == 'Z');
      CHECK(pending[kHeaderPadding] == 0xAB);
      CHECK(pending[kMarkerOffset] == 0xCD);
      const u8* image = runtime.GetMemory()->TranslateVirtual(kImageBase);
      CHECK(std::all_of(image, image + kImageSize, [](u8 byte) { return byte == 0; }));

      REQUIRE(xex->LoadContinue());
      CHECK(xex->GetPendingImage().empty());
      CHECK(std::all_of(image, image + kHeaderSize, [](u8 byte) { return byte == 0; }));
      CHECK(std::all_of(image + kMarkerOffset, image + kMarkerOffset + kMarkerSize,
                        [](u8 byte) { return byte == 0xCD; }));
    }
    rex::runtime::CloseOwned(runtime.GetObjectTable(), created);
  });
}

REX_TEST_CASE("modules: a patch changes the image before the sections are copied",
              "[runtime][module]") {
  KernelFixture fixture(PopulatePatched);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto first = modules.LoadUserModule(kPathA);
    REQUIRE(first);
    const u8* image = runtime.GetMemory()->TranslateVirtual(kImageBase);
    CHECK(std::all_of(image, image + kHeaderSize, [](u8 byte) { return byte == 0; }));
    CHECK(image[kPatchCopyOffset] == 'M');
    CHECK(image[kPatchCopyOffset + 1] == 'Z');
    CHECK(std::all_of(image + kMarkerOffset, image + kMarkerOffset + 0x10,
                      [](u8 byte) { return byte == 0; }));
    CHECK(image[kMarkerOffset + 0x10] == 0xCD);
    CHECK(CallFunction(runtime) == kResultA);
    modules.UnloadUserModule(*first);
  });
}
