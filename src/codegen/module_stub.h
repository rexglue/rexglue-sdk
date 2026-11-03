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

#include <rex/kernel/module.h>

namespace rex::codegen {

/** Module over raw binary test data held by reference. The caller owns the buffer. */
class StubModule : public kernel::Module {
 public:
  StubModule();
  ~StubModule() override = default;

  /**
   * Loads binary data for analysis.
   *
   * @param base_address Guest virtual address of the code.
   * @param data Binary data, which must outlive the module.
   */
  void Load(u32 base_address, const u8* data, size_t size);

  const std::string& GetName() const override { return name_; }
  void SetName(const std::string& name) { name_ = name; }
  bool IsExecutable() const override { return true; }
  u32 GetBaseAddress() const override { return base_address_; }
  u32 GetImageSize() const override { return size_; }
  /** Returns the entry point, which is the base address. */
  u32 GetEntryPoint() const override { return base_address_; }

 private:
  std::string name_{"test"};
  u32 base_address_ = 0;
  u32 size_ = 0;
};

class CodegenContext;

/**
 * Adds the test_ prefixed map symbols as single block functions to ctx.graph and registers
 * their call edges.
 *
 * @param ctx Codegen context whose graph is filled.
 * @param test_name Stem used to name the functions, such as "addi".
 * @param base_address Address the binary was linked at.
 */
void AnalyzeTestBinary(CodegenContext& ctx, std::string_view test_name,
                       const std::map<size_t, std::string>& symbols, u32 base_address,
                       const u8* data, size_t data_size);

}  // namespace rex::codegen
