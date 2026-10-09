/**
 * @file        tests/unit/codegen/shim_config_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/codegen/config.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

struct Scratch {
  fs::path root;

  explicit Scratch(std::string_view name)
      : root(fs::temp_directory_path() / "rexglue_shim_config_test" / fs::path(name)) {
    fs::remove_all(root);
    fs::create_directories(root);
  }

  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  std::string Write(std::string_view body) const {
    auto path = root / "game.toml";
    std::ofstream(path, std::ios::binary)
        << "file_path = \"game.xex\"\nout_directory_path = \"out\"\n"
        << body;
    return path.string();
  }
};

bool AnyError(const RecompilerConfig::ValidationResult& result, std::string_view text) {
  return std::ranges::any_of(result.errors, [text](const std::string& error) {
    return error.find(text) != std::string::npos;
  });
}

}  // namespace

TEST_CASE("Shim table loads registered names and addresses", "[codegen][shim]") {
  Scratch scratch("loads");
  RecompilerConfig config;
  REQUIRE(config.Load(scratch.Write("[shim]\nmemcpy = 0x82000000\nwcslen = 0x82000010\n")));
  CHECK(config.shimFunctions.size() == 2);
  CHECK(config.shimFunctions.at("memcpy") == 0x82000000u);
  CHECK(config.shimFunctions.at("wcslen") == 0x82000010u);
  CHECK_FALSE(config.UsesShimHeap());
}

TEST_CASE("A complete heap group turns the shim heap on", "[codegen][shim]") {
  Scratch scratch("heap");
  RecompilerConfig config;
  REQUIRE(
      config.Load(scratch.Write("[shim]\nRtlAllocateHeap = 0x82000000\n"
                                "RtlFreeHeap = 0x82000010\nRtlSizeHeap = 0x82000020\n"
                                "RtlReAllocateHeap = 0x82000030\n")));
  CHECK(config.UsesShimHeap());
}

TEST_CASE("A partial heap group is rejected", "[codegen][shim]") {
  RecompilerConfig config;
  config.filePath = "game.xex";
  config.outDirectoryPath = "out";
  config.shimFunctions["RtlAllocateHeap"] = 0x82000000;
  config.shimFunctions["RtlFreeHeap"] = 0x82000010;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "heap group is incomplete (2/4 specified)"));
  CHECK(AnyError(result, "RtlSizeHeap"));
}

TEST_CASE("An unknown shim name is rejected with the nearest name", "[codegen][shim]") {
  RecompilerConfig config;
  config.filePath = "game.xex";
  config.outDirectoryPath = "out";
  config.shimFunctions["memcopy"] = 0x82000000;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "did you mean 'memcpy'"));
}

TEST_CASE("An unknown shim name with no near match gets no suggestion", "[codegen][shim]") {
  RecompilerConfig config;
  config.filePath = "game.xex";
  config.outDirectoryPath = "out";
  config.shimFunctions["CompletelyUnrelatedName"] = 0x82000000;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "is not in the shim registry"));
  CHECK_FALSE(AnyError(result, "did you mean"));
}

TEST_CASE("A misaligned shim address is rejected", "[codegen][shim]") {
  RecompilerConfig config;
  config.filePath = "game.xex";
  config.outDirectoryPath = "out";
  config.shimFunctions["memcpy"] = 0x82000002;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(AnyError(result, "not 4-byte aligned"));
}

TEST_CASE("The retired table is rejected and leaves the shim map empty", "[codegen][shim]") {
  Scratch scratch("retired");
  RecompilerConfig config;
  CHECK_FALSE(config.Load(scratch.Write("[rexcrt]\nmemcpy = 0x82000000\n")));
  CHECK(config.shimFunctions.empty());
}
