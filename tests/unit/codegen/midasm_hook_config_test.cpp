/**
 * @file        tests/unit/codegen/midasm_hook_config_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "validation_errors.h"

#include <algorithm>
#include <string>
#include <vector>

#include <rex/codegen/config.h>

using namespace rex::codegen;

namespace {

using rex::test::HasError;

RecompilerConfig HookConfig(std::vector<std::string> registers) {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  MidAsmHook hook;
  hook.name = "TestHook";
  hook.registers = std::move(registers);
  config.mid_asm_hooks.emplace(0x82000000u, std::move(hook));
  return config;
}

}  // namespace

REX_TEST_CASE("A mid function hook cannot take r1", "[codegen][midasm]") {
  auto result = HookConfig({"r3", "r1"}).Validate();
  CHECK_FALSE(result.valid);
  CHECK(HasError(result, "TestHook"));
  CHECK(HasError(result, "r1"));
}

REX_TEST_CASE("A mid function hook may take other registers", "[codegen][midasm]") {
  auto result = HookConfig({"r3", "r11", "f1", "ctr"}).Validate();
  CHECK(result.valid);
}
