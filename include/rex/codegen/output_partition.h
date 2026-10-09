/**
 * @file        codegen/output_partition.h
 * @brief       Stable assignment of guest functions to recomp output files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::codegen {

inline constexpr std::string_view kPartitionFileName = "codegen.partition.json";

/** Function to place in an output file. */
struct FunctionSize {
  uint32_t address; /**< Function entry address */
  size_t bytes;     /**< Size of the emitted source in bytes */
};

/** Pins each guest function to a recomp file across runs. */
class OutputPartition {
 public:
  /**
   * Loads a partition from its sidecar file.
   *
   * @param path Sidecar file path
   * @return The partition, empty when the file is missing, malformed or from another version
   */
  static OutputPartition Load(const std::filesystem::path& path);

  /**
   * Assigns entries to output files. The file count is derived from `maxFileBytes` and
   * persisted, so growth alone does not reshuffle the emitted set while a new `maxFileBytes`
   * repartitions from scratch. Serialize() persists the result.
   *
   * @param entries Functions to place
   * @param maxFileBytes Size limit of an output file in bytes
   * @return One bucket per output file holding indices into `entries`
   */
  std::vector<std::vector<size_t>> Assign(std::span<const FunctionSize> entries,
                                          size_t maxFileBytes);

  /**
   * Serializes the partition as JSON for its sidecar file.
   *
   * @return JSON text ending in a newline
   */
  std::string Serialize() const;

 private:
  std::unordered_map<uint32_t, size_t> addressToFile_;
  size_t fileCount_ = 0;
  size_t maxFileBytes_ = 0;
};

}  // namespace rex::codegen
