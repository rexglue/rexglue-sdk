/**
 * @file        codegen/analysis_error_log.h
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

class AnalysisErrorLog {
 public:
  /** Failure class an Entry belongs to. */
  enum class Category : u8 {
    kUnresolvedCall, /**< A branch target that no function contains. */
    kMillicode,      /**< A branch into a millicode run off its entries, or a config
                          function at a millicode entry. */
  };

  /** An analysis failure with its addresses and message. */
  struct Entry {
    Category category;
    u32 address;           /**< Address where the failure was found */
    u32 secondary_address; /**< Address that referenced it, 0 when none */
    std::string message;
  };

  /** Adds an entry with no secondary address. */
  void Add(Category cat, u32 addr, const std::string& msg);
  /**
   * Adds an entry with a secondary address.
   *
   * @param secondary Address that referenced addr, such as a call site
   */
  void Add(Category cat, u32 addr, u32 secondary, const std::string& msg);

  /** Reports whether any entry was added. */
  bool HasErrors() const { return !entries_.empty(); }
  size_t Count() const { return entries_.size(); }
  /** Counts the entries of a failure class. */
  size_t Count(Category cat) const;

  /** Returns all entries in the order they were added. */
  const std::vector<Entry>& GetEntries() const { return entries_; }

  /** Logs every entry grouped by failure class, then the total. Logs nothing when empty. */
  void PrintReport() const;
  void Clear() { entries_.clear(); }

  /**
   * @return Static name string, "Unknown" for a value outside the enum
   */
  static const char* CategoryName(Category cat);

 private:
  std::vector<Entry> entries_;
};

}  // namespace rex::codegen
