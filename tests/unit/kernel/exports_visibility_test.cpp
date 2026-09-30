/**
 * @file        tests/unit/kernel/exports_visibility_test.cpp
 * @brief       Tests for the symbols rex-runtime exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

#include <rex/kernel/shim/registry.h>
#include <rex/platform/dynlib.h>
#include <rex/runtime/exports.h>

namespace {

using rex::runtime::ExportKind;
using rex::runtime::ExportModule;

constexpr std::array<ExportModule, 3> kModules = {ExportModule::Xboxkrnl, ExportModule::XAM,
                                                  ExportModule::XBDM};

rex::platform::DynamicLibrary OpenRuntime() {
  rex::platform::DynamicLibrary library;
  REQUIRE(library.Load(REX_TEST_RUNTIME_PATH, rex::platform::SymbolResolution::kImmediate));
  return library;
}

}  // namespace

TEST_CASE("The runtime exports every function row of the export tables", "[exports]") {
  const auto library = OpenRuntime();
  rex::u32 resolved = 0;
  for (const auto module : kModules) {
    for (const auto& entry : rex::runtime::Exports(module)) {
      if (entry.kind != ExportKind::Function) {
        continue;
      }
      const std::string symbol = "__imp__" + std::string(entry.name);
      INFO(symbol);
      CHECK(library.GetRawSymbol(symbol.c_str()) == reinterpret_cast<void*>(entry.fn));
      ++resolved;
    }
  }
  CHECK(resolved > 0);
}

TEST_CASE("The runtime exports every shim", "[exports]") {
  const auto library = OpenRuntime();
  for (const auto& entry : rex::kernel::shim::kRegistry) {
    const std::string symbol = "rexshim_" + std::string(entry.name);
    INFO(symbol);
    CHECK(library.GetRawSymbol(symbol.c_str()) != nullptr);
  }
}

TEST_CASE("The runtime exports both module entry points", "[exports]") {
  const auto library = OpenRuntime();
  CHECK(library.GetRawSymbol("rex_create_interface") != nullptr);
  CHECK(library.GetRawSymbol("rex_enumerate_interfaces") != nullptr);
}

TEST_CASE("The runtime exports none of its third party code", "[exports]") {
  const auto library = OpenRuntime();
  CHECK(library.GetRawSymbol("stbi_load_from_memory") == nullptr);
}
