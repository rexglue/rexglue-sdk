/**
 * @file        tests/unit/rexglue/test_spec_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "rexglue/commands/test_spec.h"

namespace {

std::vector<rexglue::cli::TestSpec> Parse(const std::string& text) {
  static const std::unordered_map<std::string, std::string> kSymbols = {
      {"test_a", "fn_a"}, {"test_b", "fn_b"}, {"test_c", "fn_c"}};
  std::istringstream in(text);
  return rexglue::cli::ParseTestSpecs(in, kSymbols);
}

}  // namespace

REX_TEST_CASE("A test separated by a blank line registers", "[rexglue][test_spec]") {
  const auto specs = Parse(
      "test_a:\n  #_ REGISTER_IN r3 1\n  blr\n  #_ REGISTER_OUT r3 1\n\n"
      "test_b:\n  #_ REGISTER_IN r3 2\n  blr\n  #_ REGISTER_OUT r3 2\n");
  REQUIRE(specs.size() == 2);
  CHECK(specs[0].name == "test_a");
  CHECK(specs[1].name == "test_b");
}

REX_TEST_CASE("A label right after an output block registers its own test",
              "[rexglue][test_spec]") {
  const auto specs = Parse(
      "test_a:\n  #_ REGISTER_IN r3 1\n  blr\n  #_ REGISTER_OUT r3 1\n"
      "test_b:\n  #_ REGISTER_IN r3 2\n  blr\n  #_ REGISTER_OUT r3 2\n"
      "test_c:\n  #_ REGISTER_IN r3 3\n  blr\n  #_ REGISTER_OUT r3 3\n");
  REQUIRE(specs.size() == 3);
  CHECK(specs[0].symbol == "fn_a");
  CHECK(specs[1].symbol == "fn_b");
  CHECK(specs[2].symbol == "fn_c");
  CHECK(specs[1].inputs[0].value == "2");
  CHECK(specs[1].outputs[0].value == "2");
}

REX_TEST_CASE("A plain comment in the body does not end the test", "[rexglue][test_spec]") {
  const auto specs = Parse(
      "test_a:\n  # a remark about the test\n  li r5, 0x30\n  #a remark with no space\n  blr\n"
      "  #_ REGISTER_OUT r5 0x30\n  #_ REGISTER_OUT r7 0x06\n");
  REQUIRE(specs.size() == 1);
  REQUIRE(specs[0].outputs.size() == 2);
  CHECK(specs[0].outputs[0].reg == "r5");
  CHECK(specs[0].outputs[1].value == "0x06");
}

REX_TEST_CASE("A directive with a space after the comment mark is not read",
              "[rexglue][test_spec]") {
  const auto specs = Parse("test_a:\n  # _ REGISTER_IN r3 1\n  blr\n  # _ REGISTER_OUT r3 1\n");
  CHECK(specs.empty());
}
