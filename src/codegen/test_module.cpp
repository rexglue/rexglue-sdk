/**
 * @file        codegen/test_module.cpp
 * @brief       Lightweight Module implementation for test binary loading
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/test_module.h>
#include <rex/system/binary_types.h>

namespace rex::codegen {

TestModule::TestModule() : Module(nullptr) {}

void TestModule::Load(uint32_t base_address, const uint8_t* data, size_t size) {
  base_address_ = base_address;
  size_ = static_cast<uint32_t>(size);

  // Populate binary section for FunctionScanner to read instructions
  binary_sections_.clear();
  binary_sections_.push_back(runtime::BinarySection{
      ".text", base_address, static_cast<uint32_t>(size), data,
      true,  // executable
      false  // writable
  });
}

bool TestModule::ContainsAddress(uint32_t address) {
  return address >= base_address_ && address < base_address_ + size_;
}

std::unique_ptr<runtime::Function> TestModule::CreateFunction(uint32_t /*address*/) {
  // NOTE(tomc) - this is done else where in the codegen tests,
  // and we don't need actual Function objects for the test cases, so just return null.
  // !cleanme
  return nullptr;
}

}  // namespace rex::codegen
