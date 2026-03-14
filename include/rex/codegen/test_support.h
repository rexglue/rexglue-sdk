/**
 * @file        rex/codegen/test_support.h
 * @brief       Test support utilities
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include <rex/system/module.h>

namespace rex::codegen {

/**
 * @brief Lightweight Module for loading raw binary test data
 *
 * TestModule provides the Module interface needed by FunctionScanner and
 * Recompiler without requiring a full Runtime/Processor setup. It accepts
 * raw binary data by reference (caller owns the buffer).
 *
 * Usage:
 *   std::vector<uint8_t> data = load_binary_file(...);
 *   TestModule module;
 *   module.Load(0x82010000, data.data(), data.size());
 *   recompiler.module_ = &module;
 */
class TestModule : public runtime::Module {
 public:
  TestModule();
  ~TestModule() override = default;

  /**
   * @brief Load binary data for analysis
   * @param base_address Guest virtual address where code is loaded
   * @param data Pointer to binary data (caller owns, must outlive TestModule usage)
   * @param size Size of binary data in bytes
   */
  void Load(uint32_t base_address, const uint8_t* data, size_t size);

  // Module interface overrides
  const std::string& name() const override { return name_; }
  void set_name(const std::string& name) { name_ = name; }
  bool is_executable() const override { return true; }
  uint32_t base_address() const override { return base_address_; }
  uint32_t image_size() const override { return size_; }
  uint32_t entry_point() const override { return base_address_; }
  bool ContainsAddress(uint32_t address) override;

 protected:
  std::unique_ptr<runtime::Function> CreateFunction(uint32_t address) override;

 private:
  std::string name_{"test"};
  uint32_t base_address_ = 0;
  uint32_t size_ = 0;
};

class CodegenContext;

/**
 * @brief Analyze a test binary using map symbols as function entry points.
 *
 * Filters symbols to test_ prefixed entries, adds them as functions to
 * ctx.graph with single blocks, then scans for bl instructions and
 * registers call edges.
 *
 * @param ctx         Codegen context whose graph will be populated
 * @param testName    Stem name used for function naming (e.g. "addi")
 * @param symbols     Address-to-name map from parse_map_file()
 * @param baseAddress Base address the binary was linked at
 * @param data        Pointer to raw binary data
 * @param dataSize    Size of binary data in bytes
 */
void AnalyzeTestBinary(CodegenContext& ctx, std::string_view testName,
                       const std::map<size_t, std::string>& symbols, uint32_t baseAddress,
                       const uint8_t* data, size_t dataSize);

}  // namespace rex::codegen
