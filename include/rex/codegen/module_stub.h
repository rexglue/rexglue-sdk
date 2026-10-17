/**
 * @file        codegen/module_stub.h
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

#include <rex/runtime/module.h>

namespace rex::codegen {

/**
 * @brief Lightweight Module for loading raw binary test data
 *
 * StubModule provides the Module interface needed by Recompiler without requiring a full
 * Runtime/FunctionDispatcher setup. It accepts raw binary data by reference (caller owns the
 * buffer).
 *
 * @code
 * std::vector<uint8_t> data = load_binary_file(...);
 * StubModule module;
 * module.Load(0x82010000, data.data(), data.size());
 * recompiler.module_ = &module;
 * @endcode
 */
class StubModule : public runtime::Module {
 public:
  /** Creates an empty module with no runtime. */
  StubModule();
  ~StubModule() override = default;

  /**
   * @brief Load binary data for analysis
   * @param base_address Guest virtual address where code is loaded
   * @param data Pointer to binary data (caller owns, must outlive StubModule usage)
   * @param size Size of binary data in bytes
   */
  void Load(u32 base_address, const u8* data, size_t size);

  const std::string& GetName() const override { return name_; }
  void SetName(const std::string& name) { name_ = name; }
  bool IsExecutable() const override { return true; }
  u32 GetBaseAddress() const override { return base_address_; }
  u32 GetImageSize() const override { return size_; }
  u32 GetEntryPoint() const override { return base_address_; }

 private:
  std::string name_{"test"};
  u32 base_address_ = 0;
  u32 size_ = 0;
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
 * @param test_name    Stem name used for function naming (e.g. "addi")
 * @param symbols     Address-to-name map from parse_map_file()
 * @param base_address Base address the binary was linked at
 * @param data        Pointer to raw binary data
 * @param data_size    Size of binary data in bytes
 */
void AnalyzeTestBinary(CodegenContext& ctx, std::string_view test_name,
                       const std::map<size_t, std::string>& symbols, u32 base_address,
                       const u8* data, size_t data_size);

}  // namespace rex::codegen
