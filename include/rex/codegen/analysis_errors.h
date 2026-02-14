/**
 * @file        rexcodegen/analysis_errors.h
 * @brief       Scoped error collection for analysis
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::codegen {

/**
 * Collects errors during analysis. Replaces global g_validationState.
 */
class AnalysisErrors {
 public:
  enum class Category {
    UnresolvedCall,
    MissingJumpTable,
    JumpTargetOutOfBounds,
    DiscontinuousFunction,
    UnimplementedInsn,
  };

  struct Entry {
    Category category;
    uint32_t address;
    uint32_t secondaryAddress;
    std::string message;
  };

  void Add(Category cat, uint32_t addr, const std::string& msg);
  void Add(Category cat, uint32_t addr, uint32_t secondary, const std::string& msg);

  bool HasErrors() const { return !entries_.empty(); }
  size_t Count() const { return entries_.size(); }
  size_t Count(Category cat) const;

  const std::vector<Entry>& Entries() const { return entries_; }

  void PrintReport() const;
  void Clear() { entries_.clear(); }

  static const char* CategoryName(Category cat);

 private:
  std::vector<Entry> entries_;
};

}  // namespace rex::codegen
