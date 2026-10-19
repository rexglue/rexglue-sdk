/**
 * @file        tests/unit/kernel/exports_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string_view>
#include <unordered_set>

#include <rex/runtime/exports.h>

namespace {

using namespace rex::runtime;

constexpr std::array<ExportModule, 3> kModules = {ExportModule::kXboxkrnl, ExportModule::kXAM,
                                                  ExportModule::kXBDM};

}  // namespace

REX_TEST_CASE("Export tables hold every row of the inc files", "[kernel][exports]") {
  CHECK(GetExports(ExportModule::kXboxkrnl).size() == 922);
  CHECK(GetExports(ExportModule::kXAM).size() == 1735);
  CHECK(GetExports(ExportModule::kXBDM).size() == 255);
}

REX_TEST_CASE("Export tables are sorted by unique ordinal and unique name", "[kernel][exports]") {
  for (const auto module : kModules) {
    const auto table = GetExports(module);
    std::unordered_set<std::string_view> names;
    for (size_t i = 0; i < table.size(); ++i) {
      if (i > 0) {
        REQUIRE(table[i - 1].ordinal < table[i].ordinal);
      }
      REQUIRE(names.insert(table[i].name).second);
    }
  }
}

REX_TEST_CASE("Every function row links to a translation entry and no variable does",
              "[kernel][exports]") {
  for (const auto module : kModules) {
    for (const auto& entry : GetExports(module)) {
      if (entry.kind == ExportKind::kFunction) {
        CHECK(entry.fn != nullptr);
      } else {
        CHECK(entry.fn == nullptr);
      }
    }
  }
}

REX_TEST_CASE("Ordinal lookups round trip every row", "[kernel][exports]") {
  for (const auto module : kModules) {
    for (const auto& entry : GetExports(module)) {
      REQUIRE(FindByOrdinal(module, entry.ordinal) == &entry);
    }
  }
}

REX_TEST_CASE("Lookups that miss return null", "[kernel][exports]") {
  for (const auto module : kModules) {
    CHECK(FindByOrdinal(module, 0xFFFF) == nullptr);
  }
}

REX_TEST_CASE("An ordinal in a gap of a table returns null", "[kernel][exports]") {
  for (const auto module : kModules) {
    const auto table = GetExports(module);
    for (size_t i = 1; i < table.size(); ++i) {
      if (table[i].ordinal - table[i - 1].ordinal > 1) {
        const auto gap = static_cast<rex::u16>(table[i - 1].ordinal + 1);
        REQUIRE(FindByOrdinal(module, gap) == nullptr);
        break;
      }
    }
  }
}

REX_TEST_CASE("Known kernel variables are table variables", "[kernel][exports]") {
  const auto* handle = FindByOrdinal(ExportModule::kXboxkrnl, 0x0193);
  REQUIRE(handle != nullptr);
  CHECK(handle->name == "XexExecutableModuleHandle");
  CHECK(handle->kind == ExportKind::kVariable);

  const auto* image = FindByOrdinal(ExportModule::kXboxkrnl, 0x01AF);
  REQUIRE(image != nullptr);
  CHECK(image->name == "ExLoadedImageName");
  CHECK(image->kind == ExportKind::kVariable);
}

REX_TEST_CASE("Module names map to tables in short and long form", "[kernel][exports]") {
  CHECK(ModuleFromName("xboxkrnl.exe") == ExportModule::kXboxkrnl);
  CHECK(ModuleFromName("XBOXKRNL.EXE") == ExportModule::kXboxkrnl);
  CHECK(ModuleFromName("xboxkrnl") == ExportModule::kXboxkrnl);
  CHECK(ModuleFromName("xam.xex") == ExportModule::kXAM);
  CHECK(ModuleFromName("xam") == ExportModule::kXAM);
  CHECK(ModuleFromName("xam.xex.xex") == ExportModule::kXAM);
  CHECK(ModuleFromName("xbdm.xex") == ExportModule::kXBDM);
  CHECK(ModuleFromName("Xbdm") == ExportModule::kXBDM);
}

REX_TEST_CASE("Names that are not kernel modules map to nothing", "[kernel][exports]") {
  CHECK_FALSE(ModuleFromName("").has_value());
  CHECK_FALSE(ModuleFromName("kernel32.dll").has_value());
  CHECK_FALSE(ModuleFromName("xamextra").has_value());
  CHECK_FALSE(ModuleFromName("xbox").has_value());
}

REX_TEST_CASE("Export variables bind per instance", "[kernel][exports]") {
  ExportVariables first;
  ExportVariables second;
  first.Set(0x0193, 0x40001000);
  second.Set(0x0193, 0x40002000);
  CHECK(first.Get(0x0193) == 0x40001000u);
  CHECK(second.Get(0x0193) == 0x40002000u);
  CHECK(first.Get(0x01AF) == 0u);
  first.Set(0x0193, 0x40003000);
  CHECK(first.Get(0x0193) == 0x40003000u);
  CHECK(second.Get(0x0193) == 0x40002000u);
}

REX_TEST_CASE("Exports that never return end the thread, the title or the console",
              "[kernel][exports]") {
  std::unordered_set<std::string_view> found;
  for (const auto module : kModules) {
    for (const auto& entry : GetExports(module)) {
      if (IsNoReturnExport(module, entry.ordinal)) {
        found.insert(entry.name);
      }
    }
  }
  const std::unordered_set<std::string_view> expected = {
      "ExTerminateThread",      "HalReturnToFirmware",  "KeBugCheck",
      "KeBugCheckEx",           "XamLoaderLaunchTitle", "XamLoaderLaunchTitleEx",
      "XamLoaderTerminateTitle"};
  CHECK(found == expected);
}

REX_TEST_CASE("A no return ordinal of another module does not match", "[kernel][exports]") {
  CHECK_FALSE(IsNoReturnExport(ExportModule::kXAM, 0x0019));
  CHECK_FALSE(IsNoReturnExport(ExportModule::kXboxkrnl, 0x01A4));
  CHECK_FALSE(IsNoReturnExport(ExportModule::kXBDM, 0x0019));
}
