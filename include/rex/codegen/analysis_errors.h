/**
 * @file        codegen/analysis_errors.h
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

#include <rex/types.h>

namespace rex::codegen {

/**
 * Collects errors during analysis. Replaces global g_validationState.
 */
class AnalysisErrors {
 public:
  /** Failure class an Entry belongs to. */
  enum class Category : u8 {
    UnresolvedCall,        /**< A branch target that no function contains. */
    MissingJumpTable,      /**< No analysis stage adds this class. */
    JumpTargetOutOfBounds, /**< No analysis stage adds this class. */
    DiscontinuousFunction, /**< No analysis stage adds this class. */
    UnimplementedInsn,     /**< No analysis stage adds this class. */
  };

  /** 1 analysis failure with its addresses and message. */
  struct Entry {
    Category category;    /**< Failure class */
    u32 address;          /**< Address where the failure was found */
    u32 secondaryAddress; /**< Address that referenced it, 0 when none */
    std::string message;  /**< Description of the failure */
  };

  /**
   * Adds an entry with no secondary address.
   *
   * @param cat Failure class
   * @param addr Address where the failure was found
   * @param msg Description of the failure
   */
  void Add(Category cat, u32 addr, const std::string& msg);
  /**
   * Adds an entry with a secondary address.
   *
   * @param cat Failure class
   * @param addr Address where the failure was found
   * @param secondary Address that referenced addr, such as a call site
   * @param msg Description of the failure
   */
  void Add(Category cat, u32 addr, u32 secondary, const std::string& msg);

  bool HasErrors() const { return !entries_.empty(); }
  size_t Count() const { return entries_.size(); }
  /**
   * Counts the entries of 1 failure class.
   *
   * @param cat Failure class to count
   * @return Number of entries in cat
   */
  size_t Count(Category cat) const;

  const std::vector<Entry>& Entries() const { return entries_; }

  /**
   * Logs every entry grouped by failure class, then the total. Logs nothing when empty.
   */
  void PrintReport() const;
  void Clear() { entries_.clear(); }

  /**
   * Returns the name of a failure class.
   *
   * @param cat Failure class
   * @return Static name string, "Unknown" for a value outside the enum
   */
  static const char* CategoryName(Category cat);

 private:
  std::vector<Entry> entries_;
};

}  // namespace rex::codegen
