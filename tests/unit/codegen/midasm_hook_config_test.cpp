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

#include "codegen/config.h"

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

REX_TEST_CASE("A mid function hook may take the context", "[codegen][midasm]") {
  auto result = HookConfig({"r3", "ctx"}).Validate();
  CHECK(result.valid);
}

REX_TEST_CASE("A mid function hook cannot take an unknown register", "[codegen][midasm]") {
  auto result = HookConfig({"r3", "context"}).Validate();
  CHECK_FALSE(result.valid);
  CHECK(HasError(result, "TestHook"));
  CHECK(HasError(result, "'context'"));
}

REX_TEST_CASE("A mid function hook cannot return and jump in the same form", "[codegen][midasm]") {
  RecompilerConfig config = HookConfig({});
  MidAsmHook& hook = config.mid_asm_hooks.begin()->second;
  hook.return_on_true = true;
  hook.jump_address_on_true = 0x82000100u;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(HasError(result, "TestHook"));
  CHECK(HasError(result, "'return_on_true' and 'jump_address_on_true'"));
}

REX_TEST_CASE("A mid function hook cannot mix the direct and conditional forms",
              "[codegen][midasm]") {
  RecompilerConfig config = HookConfig({});
  MidAsmHook& hook = config.mid_asm_hooks.begin()->second;
  hook.ret = true;
  hook.jump_address_on_false = 0x82000100u;
  auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(HasError(result, "TestHook"));
  CHECK(HasError(result, "direct form 'return'"));
  CHECK(HasError(result, "conditional form 'jump_address_on_false'"));
}

REX_TEST_CASE("A mid function hook with one return or one jump is valid", "[codegen][midasm]") {
  RecompilerConfig returns = HookConfig({});
  returns.mid_asm_hooks.begin()->second.ret = true;
  CHECK(returns.Validate().valid);
  RecompilerConfig conditional = HookConfig({});
  conditional.mid_asm_hooks.begin()->second.return_on_true = true;
  conditional.mid_asm_hooks.begin()->second.jump_address_on_false = 0x82000100u;
  CHECK(conditional.Validate().valid);
}
