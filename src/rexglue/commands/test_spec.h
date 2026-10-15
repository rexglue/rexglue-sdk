/**
 * @file        rexglue/commands/test_spec.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/types.h>

namespace rexglue::cli {

/** A register named by a test directive. */
struct RegValue {
  std::string reg;
  std::string value;
  bool is_vector = false;
  bool is_float = false;
  std::string vec_values[4];
};

/** A memory range named by a test directive. */
struct MemValue {
  std::string address;
  std::vector<u8> data;
};

/** The inputs and expected outputs of 1 test label. */
struct TestSpec {
  std::string name;
  std::string symbol;
  std::vector<RegValue> inputs;
  std::vector<RegValue> outputs;
  std::vector<MemValue> mem_inputs;
  std::vector<MemValue> mem_outputs;
};

/**
 * Builds the test name shown by the test runner.
 *
 * @param stem The assembly file name without its extension.
 * @param label The test label.
 * @return The group, a dot and the test name.
 */
std::string BuildHierarchicalName(const std::string& stem, const std::string& label);

/**
 * Reads the tests of an assembly source.
 *
 * A label starts a test when the symbol table names it and its comment lines carry input or output
 * directives.
 *
 * @param in The assembly source.
 * @param symbols The test function of each label.
 * @return The tests in source order.
 */
std::vector<TestSpec> ParseTestSpecs(std::istream& in,
                                     const std::unordered_map<std::string, std::string>& symbols);

}  // namespace rexglue::cli
