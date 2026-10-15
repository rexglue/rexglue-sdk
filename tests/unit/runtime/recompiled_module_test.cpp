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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/thread_state.h>

#include "core/xex_builder.h"
#include "pe/pe_image.h"
#include "kernel/kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u32 kImageBase = 0x8E000000;
constexpr u32 kFunctionAddress = kImageBase + 0x100;
constexpr u32 kResultA = 0xA1;
constexpr const char* kPathA = "game:\\a.xex";
constexpr const char* kPathB = "game:\\b.xex";

constexpr u32 kImageSize = 0x10000;

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
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = 0x1000;
  section->Misc.VirtualSize = 0x200;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  return image;
}

void WriteImage(const std::filesystem::path& root, const char* name) {
  rex::test::XEXSpec spec;
  spec.load_address = kImageBase;
  spec.page_size_4kb = false;
  spec.image = BuildPEImage();
  const auto file = rex::test::BuildXEX(spec);
  std::ofstream out(root / name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void PopulateImages(const std::filesystem::path& root) {
  WriteImage(root, "a.xex");
  WriteImage(root, "b.xex");
}

void RegisterImages(rex::Runtime& runtime) {
  auto& modules = runtime.GetModuleRegistry();
  modules.RegisterRecompiledModule("a", kPathA, REX_TEST_RECOMPILED_A_PATH);
  modules.RegisterRecompiledModule("b", kPathB, REX_TEST_RECOMPILED_B_PATH);
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
