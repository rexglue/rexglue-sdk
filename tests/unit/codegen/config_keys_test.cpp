/**
 * @file        tests/unit/codegen/config_keys_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "file_text.h"
#include "temp_folder.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/config.h"

using rex::codegen::RecompilerConfig;
using rex::codegen::VectorMath;
using rex::test::WriteText;

namespace {

constexpr const char* kBase = "file_path = \"game.xex\"\nout_directory_path = \"out\"\n";

bool Mentions(const std::vector<std::string>& warnings, std::string_view text) {
  return std::ranges::any_of(warnings, [text](const std::string& warning) {
    return warning.find(text) != std::string::npos;
  });
}

struct Loaded {
  RecompilerConfig config;
  bool ok = false;
};

Loaded LoadBody(std::string_view body) {
  rex::test::TempFolder folder("rexglue_config_keys_");
  WriteText(folder.path / "game.toml", std::string(kBase) + std::string(body));
  Loaded loaded;
  loaded.ok = loaded.config.Load((folder.path / "game.toml").string());
  return loaded;
}

}  // namespace

REX_TEST_CASE("An unknown top level key warns with its name and file", "[codegen][config][keys]") {
  const auto loaded = LoadBody("skip_lrr = true\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "skip_lrr"));
  CHECK(Mentions(loaded.config.load_warnings, "game.toml"));
}

REX_TEST_CASE("A key with the wrong type warns and the load still succeeds",
              "[codegen][config][keys]") {
  const auto loaded = LoadBody("skip_lr = \"yes\"\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "skip_lr"));
  CHECK(Mentions(loaded.config.load_warnings, "boolean"));
}

REX_TEST_CASE("An unknown key inside a table warns with the full key path",
              "[codegen][config][keys]") {
  const auto loaded = LoadBody("[analysis]\nbogus = 1\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "analysis.bogus"));
}

REX_TEST_CASE("An unknown key in a function entry warns with the address",
              "[codegen][config][keys]") {
  const auto loaded = LoadBody("[functions]\n0x82000000 = { nam = \"x\" }\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "functions.0x82000000.nam"));
}

REX_TEST_CASE("An unknown key in an array entry warns with the entry number",
              "[codegen][config][keys]") {
  const auto loaded =
      LoadBody("[[midasm_hook]]\naddress = 0x82000000\nname = \"hook\"\nadress = 1\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "midasm_hook[0].adress"));
}

REX_TEST_CASE("Keys of removed settings warn as unknown", "[codegen][config][keys]") {
  const auto loaded = LoadBody(
      "indirect_calls = [0x82000010]\n[analysis]\nmax_jump_extension = 1\n"
      "large_function_threshold = 1\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "indirect_calls"));
  CHECK(Mentions(loaded.config.load_warnings, "analysis.max_jump_extension"));
  CHECK(Mentions(loaded.config.load_warnings, "analysis.large_function_threshold"));
}

REX_TEST_CASE("A table that uses only known keys has no warnings", "[codegen][config][keys]") {
  const auto loaded = LoadBody(
      "project_name = \"demo\"\ntemplate_dir = \"t\"\nlongjmp_address = 0x82000000\n"
      "setjmp_address = 0x82000004\nskip_lr = true\nskip_msr = true\nctr_as_local = true\n"
      "xer_as_local = true\nreserved_as_local = true\ncr_as_local = true\n"
      "non_argument_as_local = true\nnon_volatile_as_local = true\nforward_stack_stores = true\n"
      "generate_exception_handlers = true\nis_dll = false\n"
      "[analysis]\ndata_region_threshold = 1\n"
      "exception_handler_funcs = [0x82000020]\n"
      "[functions]\n0x82000030 = { size = 8, name = \"f\", share_registers = true }\n"
      "[[invalid_instructions]]\ndata = 0x82000040\nsize = 4\n"
      "[[switch_tables]]\naddress = 0x82000050\nregister = 11\nlabels = [0x82000054]\n"
      "[[midasm_hook]]\naddress = 0x82000060\nname = \"h\"\nregisters = [\"r3\"]\nreturn = true\n"
      "[mmio.gpu]\nread = [0x82000070]\nwrite = [0x82000074]\n"
      "[mmio.apu]\nread = [0x82000078]\nwrite = [0x8200007c]\n"
      "[vector_math]\nvmaddfp = \"exact\"\nvnmsubfp = \"fast\"\nvmsum3fp128 = \"exact\"\n"
      "vmsum4fp128 = \"exact\"\nvrefp = \"fast\"\nvrsqrtefp = \"fast\"\nvexptefp = \"exact\"\n"
      "fres = \"fast\"\nfrsqrte = \"exact\"\n");
  CHECK(loaded.config.load_warnings.empty());
}

REX_TEST_CASE("A vector math key set to exact applies only to its instruction",
              "[codegen][config][keys]") {
  const auto defaults = LoadBody("");
  const auto& dm = defaults.config.vector_math;
  CHECK(dm.vmaddfp == VectorMath::kFast);
  CHECK(dm.vnmsubfp == VectorMath::kFast);
  CHECK(dm.vmsum3fp128 == VectorMath::kFast);
  CHECK(dm.vmsum4fp128 == VectorMath::kFast);
  CHECK(dm.vrefp == VectorMath::kFast);
  CHECK(dm.vrsqrtefp == VectorMath::kFast);
  CHECK(dm.vexptefp == VectorMath::kFast);
  CHECK(dm.fres == VectorMath::kFast);
  CHECK(dm.frsqrte == VectorMath::kFast);

  const auto loaded = LoadBody("[vector_math]\nvmsum4fp128 = \"exact\"\nvrefp = \"fast\"\n");
  CHECK(loaded.config.load_warnings.empty());
  const auto& vm = loaded.config.vector_math;
  CHECK(vm.vmaddfp == VectorMath::kFast);
  CHECK(vm.vnmsubfp == VectorMath::kFast);
  CHECK(vm.vmsum3fp128 == VectorMath::kFast);
  CHECK(vm.vmsum4fp128 == VectorMath::kExact);
  CHECK(vm.vrefp == VectorMath::kFast);
  CHECK(vm.vrsqrtefp == VectorMath::kFast);
  CHECK(vm.vexptefp == VectorMath::kFast);
  CHECK(vm.fres == VectorMath::kFast);
  CHECK(vm.frsqrte == VectorMath::kFast);
}

REX_TEST_CASE("A vector math value other than fast or exact warns and keeps fast",
              "[codegen][config][keys]") {
  const auto loaded = LoadBody("[vector_math]\nvexptefp = \"precise\"\n");
  CHECK(loaded.ok);
  CHECK(Mentions(loaded.config.load_warnings, "vector_math.vexptefp"));
  CHECK(loaded.config.vector_math.vexptefp == VectorMath::kFast);
}

REX_TEST_CASE("A vector math key for an instruction without a fast form warns",
              "[codegen][config][keys]") {
  const auto loaded = LoadBody("[vector_math]\nvlogefp = \"fast\"\n");
  CHECK(Mentions(loaded.config.load_warnings, "vector_math.vlogefp"));
}
