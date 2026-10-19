/**
 * @file        tests/unit/runtime/cross_module_import_test.cpp
 * @brief       A module imports functions and variables by ordinal from another user module
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
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem/xex.h>
#include <rex/logging/logger.h>
#include <rex/logging/sink.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/module_registry.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

#include "core/xex_builder.h"
#include "kernel/kernel_fixture.h"
#include "pe/pe_image.h"
#include "runtime/module_registry.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr u32 kExporterBase = 0x8E000000;
constexpr u32 kImporterBase = 0x8C000000;
constexpr u32 kImageSize = 0x10000;
constexpr u32 kHeaderSize = 0x1000;
constexpr u32 kHeaderPadding = 0x400;
constexpr u32 kExportTableOffset = 0x2000;
constexpr u32 kDataOffset = 0x3000;
constexpr u32 kDataValue = 0xCAFE1234;
constexpr u32 kFunctionOrdinal = 1;
constexpr u32 kVariableOrdinal = 2;
constexpr u32 kFunctionSlot = kImporterBase + 0x2000;
constexpr u32 kVariableSlot = kImporterBase + 0x2008;
constexpr u32 kThunkSlot = kImporterBase + 0x2010;
constexpr u32 kSecondSlot = kImporterBase + 0x2018;
constexpr u32 kKernelSlot = kImporterBase + 0x2020;
constexpr u32 kKernelThunk = kImporterBase + 0x2028;
constexpr u32 kKernelOrdinal = 0xCF;
constexpr u32 kIndirectKernelFunction = kImporterBase + 0x300;
constexpr u32 kKernelResult = 0xB2;
constexpr u32 kExporterFunction = kExporterBase + 0x100;
constexpr u32 kDirectFunction = kImporterBase + 0x100;
constexpr u32 kIndirectFunction = kImporterBase + 0x200;
constexpr u32 kResult = 0xA1;
constexpr const char* kExporterPath = "game:\\exporter.xex";
constexpr const char* kImporterPath = "game:\\importer.xex";

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
  nt->OptionalHeader.SizeOfHeaders = kHeaderSize;
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = kHeaderSize;
  section->Misc.VirtualSize = 0x200;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  std::memset(image.data() + kHeaderPadding, 0xAB, 0x40);
  return image;
}

void Write(const std::filesystem::path& path, const rex::test::XEXSpec& spec) {
  const auto file = rex::test::BuildXEX(spec);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void WriteExporter(const std::filesystem::path& root,
                   u32 module_flags = rex::filesystem::kXEXModuleTitle) {
  rex::test::XEXSpec spec;
  spec.module_flags = module_flags;
  spec.load_address = kExporterBase;
  spec.page_size_4kb = false;
  spec.image = BuildPEImage();
  spec.export_table = kExporterBase + kExportTableOffset;
  rex::test::PutBE32(spec.image, kExportTableOffset + 32, kExporterBase >> 16);
  rex::test::PutBE32(spec.image, kExportTableOffset + 36, 2);
  rex::test::PutBE32(spec.image, kExportTableOffset + 40, 1);
  rex::test::PutBE32(spec.image, kExportTableOffset + 44, kExporterFunction - kExporterBase);
  rex::test::PutBE32(spec.image, kExportTableOffset + 48, kDataOffset);
  rex::test::PutBE32(spec.image, kDataOffset, kDataValue);
  Write(root / "exporter.xex", spec);
}

void WriteImporter(const std::filesystem::path& root, const char* file, u32 ordinal,
                   bool second_library = false) {
  rex::test::XEXSpec spec;
  spec.load_address = kImporterBase;
  spec.page_size_4kb = false;
  spec.image = BuildPEImage();
  rex::test::PutBE32(spec.image, 0x2000, ordinal);
  rex::test::PutBE32(spec.image, 0x2008, kVariableOrdinal);
  rex::test::PutBE32(spec.image, 0x2010, (1u << 24) | ordinal);
  rex::test::PutBE32(spec.image, 0x2020, kKernelOrdinal);
  rex::test::PutBE32(spec.image, 0x2028, (1u << 24) | kKernelOrdinal);
  spec.imports.push_back({"exporter.dll", {kFunctionSlot, kVariableSlot, kThunkSlot}, 0});
  spec.imports.push_back({"xboxkrnl.exe", {kKernelSlot, kKernelThunk}, 0});
  if (second_library) {
    rex::test::PutBE32(spec.image, 0x2018, kFunctionOrdinal);
    spec.imports.push_back({"absent.dll", {kSecondSlot}, 0});
  }
  Write(root / file, spec);
}

void PopulateAll(const std::filesystem::path& root) {
  WriteExporter(root);
  WriteImporter(root, "importer.xex", kFunctionOrdinal);
}

void PopulateDLLExporter(const std::filesystem::path& root) {
  WriteExporter(root, rex::filesystem::kXEXModuleDLLModule);
  WriteImporter(root, "importer.xex", kFunctionOrdinal);
}

void PopulateWithoutExporter(const std::filesystem::path& root) {
  WriteImporter(root, "importer.xex", kFunctionOrdinal);
}

void PopulateSecondLibraryMissing(const std::filesystem::path& root) {
  WriteExporter(root);
  WriteImporter(root, "importer.xex", kFunctionOrdinal, true);
}

void PopulateMissingOrdinal(const std::filesystem::path& root) {
  WriteExporter(root);
  WriteImporter(root, "importer.xex", 9);
}

void RegisterImages(rex::Runtime& runtime) {
  auto& modules = runtime.GetModuleRegistry();
  modules.RegisterRecompiledModule("exporter", kExporterPath, REX_TEST_CROSS_EXPORTER_NAME);
  modules.RegisterRecompiledModule("importer", kImporterPath, REX_TEST_CROSS_IMPORTER_NAME);
}

u32 CallFunction(rex::Runtime& runtime, u32 address) {
  u64 args[1] = {0};
  return static_cast<u32>(
      runtime.GetFunctionDispatcher()->Execute(rex::runtime::ThreadState::Get(), address, args, 0));
}

void UnloadBoth(rex::Runtime& runtime, rex::X_HANDLE importer) {
  auto& modules = runtime.GetModuleRegistry();
  modules.UnloadUserModule(importer);
  if (auto exporter = modules.GetModule(kExporterPath)) {
    modules.UnloadUserModule(*exporter);
  }
}

}  // namespace

REX_TEST_CASE("loading a module loads its exporter and patches the import slots",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    CHECK_FALSE(modules.GetModule(kExporterPath));
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE(importer);
    CHECK(modules.GetModule(kExporterPath));

    CHECK(c.Load32(kFunctionSlot) == kExporterFunction);
    const u32 variable = c.Load32(kVariableSlot);
    CHECK(variable == kExporterBase + kDataOffset);
    CHECK(c.Load32(variable) == kDataValue);
    CHECK(c.Load32(kThunkSlot) == ((1u << 24) | kFunctionOrdinal));

    const u8* header = runtime.GetMemory()->TranslateVirtual(kExporterBase);
    CHECK(std::all_of(header, header + kHeaderSize, [](u8 byte) { return byte == 0; }));
    CHECK(c.Kernel("RtlImageNtHeader", {kExporterBase}) == 0);

    UnloadBoth(runtime, *importer);
  });
}

REX_TEST_CASE("the generated style call reaches the exporter function",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto importer = runtime.GetModuleRegistry().LoadUserModule(kImporterPath);
    REQUIRE(importer);
    CHECK(CallFunction(runtime, kDirectFunction) == kResult);
    UnloadBoth(runtime, *importer);
  });
}

REX_TEST_CASE("an indirect call through a function slot reaches the exporter function",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto importer = runtime.GetModuleRegistry().LoadUserModule(kImporterPath);
    REQUIRE(importer);
    CHECK(CallFunction(runtime, kIndirectFunction) == kResult);
    UnloadBoth(runtime, *importer);
  });
}

REX_TEST_CASE("a missing exporter fails the load with DLL not found", "[runtime][module][import]") {
  KernelFixture fixture(PopulateWithoutExporter);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto importer = runtime.GetModuleRegistry().LoadUserModule(kImporterPath);
    REQUIRE_FALSE(importer);
    CHECK(rex::runtime::ToXStatus(importer.error()) == rex::X_STATUS_DLL_NOT_FOUND);
    CHECK_FALSE(runtime.GetModuleRegistry().GetModule(kImporterPath));
  });
}

REX_TEST_CASE("an ordinal the exporter lacks fails the load with ordinal not found",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateMissingOrdinal);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE_FALSE(importer);
    CHECK(rex::runtime::ToXStatus(importer.error()) == rex::X_STATUS_DRIVER_ORDINAL_NOT_FOUND);
    CHECK_FALSE(modules.GetModule(kImporterPath));
    if (auto exporter = modules.GetModule(kExporterPath)) {
      modules.UnloadUserModule(*exporter);
    }
  });
}

REX_TEST_CASE("the registry generation changes when a module loads or unloads",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    const auto& registry = static_cast<const rex::runtime::ModuleRegistry&>(modules);
    const u64 before = registry.GetGeneration();
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE(importer);
    const u64 loaded = registry.GetGeneration();
    CHECK(loaded > before);
    modules.UnloadUserModule(*importer);
    CHECK(registry.GetGeneration() > loaded);
    if (auto exporter = modules.GetModule(kExporterPath)) {
      modules.UnloadUserModule(*exporter);
    }
  });
}

REX_TEST_CASE("a failed load releases the exporter it loaded", "[runtime][module][import]") {
  KernelFixture fixture(PopulateSecondLibraryMissing);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE_FALSE(importer);
    CHECK(rex::runtime::ToXStatus(importer.error()) == rex::X_STATUS_DLL_NOT_FOUND);
    CHECK_FALSE(modules.GetModule(kImporterPath));
    CHECK_FALSE(modules.GetModule(kExporterPath));
  });
}

REX_TEST_CASE("unloading the importer unloads an exporter loaded for it",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE(importer);
    REQUIRE(modules.GetModule(kExporterPath));
    modules.UnloadUserModule(*importer);
    CHECK_FALSE(modules.GetModule(kImporterPath));
    CHECK_FALSE(modules.GetModule(kExporterPath));
  });
}

REX_TEST_CASE("a codegen unload of the importer attempts no detach on its exporter",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateDLLExporter);
  auto* logger = fixture.GetAPI().Get<rex::ILogger>();
  REQUIRE(logger);
  rex::log::CaptureSink sink;
  logger->AddSink(sink);
  auto& modules = fixture.GetRuntime().GetModuleRegistry();
  rex::X_HANDLE importer = 0;
  fixture.RunOnGuestThread([&](Caller& c) {
    RegisterImages(c.GetRuntime());
    auto loaded = modules.LoadUserModule(kImporterPath, false);
    REQUIRE(loaded);
    importer = *loaded;
  });
  modules.UnloadUserModule(importer, false);
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  logger->RemoveSink(sink);
  CHECK_FALSE(modules.GetModule(kExporterPath));
  CHECK_FALSE(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text.starts_with("DllMain(DLL_PROCESS_DETACH) skipped");
  }));
}

REX_TEST_CASE("an exporter the title loaded stays loaded after the importer unloads",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto& modules = runtime.GetModuleRegistry();
    auto exporter = modules.LoadUserModule(kExporterPath);
    REQUIRE(exporter);
    auto importer = modules.LoadUserModule(kImporterPath);
    REQUIRE(importer);
    modules.UnloadUserModule(*importer);
    CHECK_FALSE(modules.GetModule(kImporterPath));
    CHECK(modules.GetModule(kExporterPath));
    modules.UnloadUserModule(*exporter);
  });
}

REX_TEST_CASE("a kernel function slot holds the thunk address and an indirect call reaches it",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto importer = runtime.GetModuleRegistry().LoadUserModule(kImporterPath);
    REQUIRE(importer);
    CHECK(c.Load32(kKernelSlot) == kKernelThunk);
    CHECK(CallFunction(runtime, kIndirectKernelFunction) == kKernelResult);
    UnloadBoth(runtime, *importer);
  });
}

REX_TEST_CASE("a kernel import record patches the thunk address into its slot",
              "[runtime][module][import]") {
  KernelFixture fixture(PopulateAll);
  fixture.RunOnGuestThread([](Caller& c) {
    auto& runtime = c.GetRuntime();
    RegisterImages(runtime);
    auto importer = runtime.GetModuleRegistry().LoadUserModule(kImporterPath);
    REQUIRE(importer);
    CHECK(c.Load32(kKernelSlot) == kKernelThunk);
    CHECK(c.Load32(kKernelThunk) == ((1u << 24) | kKernelOrdinal));
    UnloadBoth(runtime, *importer);
  });
}
