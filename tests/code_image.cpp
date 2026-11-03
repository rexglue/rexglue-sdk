#include "code_image.h"

#include "object_app.h"
#include "rex_test.h"
#include "temp_folder.h"
#include "codegen/tool_runtime.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "codegen/manifest.h"
#include "codegen/project_recompiler.h"
#include <rex/filesystem/xex.h>
#include <rex/logging/logger.h>

#include "base/xex_builder.h"
#include "pe/pe_image.h"
#include "rexglue/log_categories.h"
#include "test_memory.h"

namespace rex::test {

std::vector<u8> BuildCodeImage(std::span<const u32> words, std::span<const PDataEntry> pdata,
                               std::span<const u32> rdata) {
  std::vector<u8> image(kCodeImageSize, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = rdata.empty() ? 2 : 3;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = kCodeImageCodeOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress =
      kCodeImagePDataOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size =
      static_cast<u32>(pdata.size() * 8);
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = kCodeImageCodeOffset;
  section->Misc.VirtualSize = std::max<u32>(0x200, static_cast<u32>(words.size() * 4));
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  auto* pdata_section = section + 1;
  std::memcpy(pdata_section->Name, ".pdata", 6);
  pdata_section->VirtualAddress = kCodeImagePDataOffset;
  pdata_section->Misc.VirtualSize = 0x100;
  if (!rdata.empty()) {
    auto* rdata_section = pdata_section + 1;
    std::memcpy(rdata_section->Name, ".rdata", 6);
    rdata_section->VirtualAddress = kCodeImageRDataOffset;
    rdata_section->Misc.VirtualSize = std::max<u32>(0x100, static_cast<u32>(rdata.size() * 4));
    u32 rdata_offset = kCodeImageRDataOffset;
    for (u32 word : rdata) {
      PutBE32(image, rdata_offset, word);
      rdata_offset += 4;
    }
  }
  u32 offset = kCodeImagePDataOffset;
  for (const auto& entry : pdata) {
    PutBE32(image, offset, entry.begin);
    PutBE32(image, offset + 4, entry.prolog | (entry.length << 8) | entry.flags);
    offset += 8;
  }
  offset = kCodeImageCodeOffset;
  for (u32 word : words) {
    PutBE32(image, offset, word);
    offset += 4;
  }
  return image;
}

std::string ReadGeneratedTree(const std::filesystem::path& root) {
  std::string text;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return text;
}

Result<void> GenerateFromImage(const TempFolder& folder, std::vector<u8> image,
                               std::string_view extra_manifest, bool force) {
  return GenerateFromImage(folder, std::move(image), extra_manifest, force, {}, nullptr, {});
}

Result<void> GenerateFromImage(const TempFolder& folder, std::vector<u8> image,
                               std::string_view extra_manifest, bool force,
                               std::vector<XEXImportSpec> imports, log::ISink* sink,
                               std::string_view project_keys) {
  XEXSpec spec;
  spec.load_address = kCodeImageBase;
  spec.entry_point = kCodeImageBase + kCodeImageCodeOffset;
  spec.page_size_4kb = false;
  spec.image = std::move(image);
  spec.imports = std::move(imports);
  const auto file = BuildXEX(spec);
  std::ofstream(folder.path / "entry.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << "[project]\nname = \"demo\"\n"
                               << project_keys << R"(
[entrypoint]
file_path = "entry.xex"
out_directory_path = "generated/entry"
)" << extra_manifest;
  const auto manifest = codegen::ManifestConfig::Load(manifest_path);
  if (!manifest) {
    return Err<void>(ErrorCategory::kConfig, "the manifest did not load");
  }

  test::ReleaseTestMemory();
  Logger& logger = REX_LOGGER();
  REQUIRE(logger.debugger.Set(false));
  ScopeGuard sink_guard;
  if (sink) {
    sink_guard = logger.AddSink(*sink);
  }
  REQUIRE(logger.AddCategories(rexglue::kLogCategories));
  codegen::ProjectRecompiler recompiler(*manifest);
  test::TempFolder tool_root("rex_recompiler_tool_");
  test::ObjectApp<codegen::ToolApp> tools("recompiler_test", tool_root.path);
  codegen::ProjectRecompilerOptions options;
  options.tools = &tools;
  options.force = force;
  auto result = recompiler.Run(options);
  logger.RemoveCategories(rexglue::kLogCategories);
  if (sink) {
    sink_guard.Reset();
  }
  return result;
}

}  // namespace rex::test
