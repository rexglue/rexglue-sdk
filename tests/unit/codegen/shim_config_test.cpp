/**
 * @file        tests/unit/codegen/shim_config_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"
#include "support/validation_errors.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/codegen/config.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

using rex::test::AnyError;

struct Scratch {
  rex::test::TempFolder folder;
  fs::path root;

  explicit Scratch(std::string_view name)
      : folder(std::string("rexglue_shim_config_") + std::string(name) + "_"), root(folder.path) {}

  std::string Write(std::string_view body) const {
    auto path = root / "game.toml";
    std::ofstream(path, std::ios::binary)
        << "file_path = \"game.xex\"\nout_directory_path = \"out\"\n"
        << body;
    return path.string();
  }
};

}  // namespace

REX_TEST_CASE("Shim table loads registered names and addresses", "[codegen][shim]") {
  Scratch scratch("loads");
  RecompilerConfig config;
  REQUIRE(config.Load(scratch.Write("[shim]\nmemcpy = 0x82000000\nwcslen = 0x82000010\n")));
  CHECK(config.shim_functions.size() == 2);
  CHECK(config.shim_functions.at("memcpy") == 0x82000000u);
  CHECK(config.shim_functions.at("wcslen") == 0x82000010u);
  CHECK_FALSE(config.UsesShimHeap());
}

REX_TEST_CASE("A complete heap group turns the shim heap on", "[codegen][shim]") {
  Scratch scratch("heap");
  RecompilerConfig config;
  REQUIRE(
      config.Load(scratch.Write("[shim]\nRtlAllocateHeap = 0x82000000\n"
                                "RtlFreeHeap = 0x82000010\nRtlSizeHeap = 0x82000020\n"
                                "RtlReAllocateHeap = 0x82000030\n")));
  CHECK(config.UsesShimHeap());
}

REX_TEST_CASE("A partial heap group is rejected", "[codegen][shim]") {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  config.shim_functions["RtlAllocateHeap"] = 0x82000000;
  config.shim_functions["RtlFreeHeap"] = 0x82000010;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "heap group is incomplete (2/4 specified)"));
  CHECK(AnyError(result, "RtlSizeHeap"));
}

REX_TEST_CASE("An unknown shim name is rejected with the nearest name", "[codegen][shim]") {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  config.shim_functions["memcopy"] = 0x82000000;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "did you mean 'memcpy'"));
}

REX_TEST_CASE("An unknown shim name with no near match gets no suggestion", "[codegen][shim]") {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  config.shim_functions["CompletelyUnrelatedName"] = 0x82000000;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "is not in the shim registry"));
  CHECK_FALSE(AnyError(result, "did you mean"));
}

REX_TEST_CASE("A misaligned shim address is rejected", "[codegen][shim]") {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  config.shim_functions["memcpy"] = 0x82000002;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "not 4-byte aligned"));
}

REX_TEST_CASE("The retired table is rejected and leaves the shim map empty", "[codegen][shim]") {
  Scratch scratch("retired");
  RecompilerConfig config;
  CHECK_FALSE(config.Load(scratch.Write("[rexcrt]\nmemcpy = 0x82000000\n")));
  CHECK(config.shim_functions.empty());
}
