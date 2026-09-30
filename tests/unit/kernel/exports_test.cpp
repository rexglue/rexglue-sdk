/**
 * @file        tests/unit/kernel/exports_test.cpp
 * @brief       Tests for the kernel export tables
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <unordered_set>

#include <rex/runtime/exports.h>

namespace {

using namespace rex::runtime;

constexpr std::array<ExportModule, 3> kModules = {ExportModule::Xboxkrnl, ExportModule::XAM,
                                                  ExportModule::XBDM};

}  // namespace

TEST_CASE("Export tables hold every row of the inc files", "[kernel][exports]") {
  CHECK(Exports(ExportModule::Xboxkrnl).size() == 922);
  CHECK(Exports(ExportModule::XAM).size() == 1735);
  CHECK(Exports(ExportModule::XBDM).size() == 255);
}

TEST_CASE("Export tables are sorted by unique ordinal and unique name", "[kernel][exports]") {
  for (const auto module : kModules) {
    const auto table = Exports(module);
    std::unordered_set<std::string_view> names;
    for (size_t i = 0; i < table.size(); ++i) {
      if (i > 0) {
        REQUIRE(table[i - 1].ordinal < table[i].ordinal);
      }
      REQUIRE(names.insert(table[i].name).second);
    }
  }
}

TEST_CASE("Every function row links to a translation entry and no variable does",
          "[kernel][exports]") {
  for (const auto module : kModules) {
    for (const auto& entry : Exports(module)) {
      if (entry.kind == ExportKind::Function) {
        CHECK(entry.fn != nullptr);
      } else {
        CHECK(entry.fn == nullptr);
      }
    }
  }
}

TEST_CASE("Lookups round trip every row", "[kernel][exports]") {
  for (const auto module : kModules) {
    for (const auto& entry : Exports(module)) {
      REQUIRE(FindByOrdinal(module, entry.ordinal) == &entry);
      REQUIRE(FindByName(module, entry.name) == &entry);
    }
  }
}

TEST_CASE("Lookups that miss return null", "[kernel][exports]") {
  for (const auto module : kModules) {
    CHECK(FindByOrdinal(module, 0xFFFF) == nullptr);
    CHECK(FindByName(module, "") == nullptr);
    CHECK(FindByName(module, "NoSuchExportName") == nullptr);
  }
}

TEST_CASE("An ordinal in a gap of a table returns null", "[kernel][exports]") {
  for (const auto module : kModules) {
    const auto table = Exports(module);
    for (size_t i = 1; i < table.size(); ++i) {
      if (table[i].ordinal - table[i - 1].ordinal > 1) {
        const auto gap = static_cast<rex::u16>(table[i - 1].ordinal + 1);
        REQUIRE(FindByOrdinal(module, gap) == nullptr);
        break;
      }
    }
  }
}

TEST_CASE("Known kernel variables are table variables", "[kernel][exports]") {
  const auto* handle = FindByOrdinal(ExportModule::Xboxkrnl, 0x0193);
  REQUIRE(handle != nullptr);
  CHECK(handle->name == "XexExecutableModuleHandle");
  CHECK(handle->kind == ExportKind::Variable);

  const auto* image = FindByOrdinal(ExportModule::Xboxkrnl, 0x01AF);
  REQUIRE(image != nullptr);
  CHECK(image->name == "ExLoadedImageName");
  CHECK(image->kind == ExportKind::Variable);
}

TEST_CASE("Module names map to tables in short and long form", "[kernel][exports]") {
  CHECK(ModuleFromName("xboxkrnl.exe") == ExportModule::Xboxkrnl);
  CHECK(ModuleFromName("XBOXKRNL.EXE") == ExportModule::Xboxkrnl);
  CHECK(ModuleFromName("xboxkrnl") == ExportModule::Xboxkrnl);
  CHECK(ModuleFromName("xam.xex") == ExportModule::XAM);
  CHECK(ModuleFromName("xam") == ExportModule::XAM);
  CHECK(ModuleFromName("xam.xex.xex") == ExportModule::XAM);
  CHECK(ModuleFromName("xbdm.xex") == ExportModule::XBDM);
  CHECK(ModuleFromName("Xbdm") == ExportModule::XBDM);
}

TEST_CASE("Names that are not kernel modules map to nothing", "[kernel][exports]") {
  CHECK_FALSE(ModuleFromName("").has_value());
  CHECK_FALSE(ModuleFromName("kernel32.dll").has_value());
  CHECK_FALSE(ModuleFromName("xamextra").has_value());
  CHECK_FALSE(ModuleFromName("xbox").has_value());
}

TEST_CASE("Export variables bind per instance", "[kernel][exports]") {
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
