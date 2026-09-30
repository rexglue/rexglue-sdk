/**
 * @file        codegen/output_partition.cpp
 * @brief       Stable assignment of guest functions to recomp output files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/output_partition.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rex/logging.h>

namespace rex::codegen {

namespace {

/// Bump when the sidecar layout changes.
constexpr int kPartitionVersion = 2;

size_t ChooseFileCount(std::span<const FunctionSize> entries, size_t maxFileBytes) {
  if (maxFileBytes == 0)
    return 1;
  size_t total = 0;
  for (const auto& entry : entries) {
    total += entry.bytes;
  }
  return std::max<size_t>(1, (total + maxFileBytes - 1) / maxFileBytes);
}

}  // namespace

OutputPartition OutputPartition::Load(const std::filesystem::path& path) {
  OutputPartition partition;

  std::ifstream in(path, std::ios::binary);
  if (!in)
    return partition;

  nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    REXLOG_WARN(codegen, "Ignoring malformed partition sidecar: {}", path.string());
    return partition;
  }

  auto version = doc.find("version");
  if (version == doc.end() || !version->is_number_integer() ||
      version->get<int>() != kPartitionVersion) {
    REXLOG_DEBUG(codegen, "Ignoring partition sidecar from another version: {}", path.string());
    return partition;
  }

  if (auto it = doc.find("file_count"); it != doc.end() && it->is_number_unsigned())
    partition.fileCount_ = it->get<size_t>();
  if (auto it = doc.find("max_file_bytes"); it != doc.end() && it->is_number_unsigned())
    partition.maxFileBytes_ = it->get<size_t>();

  auto it = doc.find("assignments");
  if (it == doc.end() || !it->is_object())
    return partition;

  for (const auto& [key, value] : it->items()) {
    if (!value.is_number_unsigned())
      continue;
    uint32_t address = 0;
    const char* first = key.data();
    const char* last = first + key.size();
    auto [ptr, ec] = std::from_chars(first, last, address, 16);
    if (ec != std::errc{} || ptr != last)
      continue;
    partition.addressToFile_[address] = value.get<size_t>();
  }
  return partition;
}

std::string OutputPartition::Serialize() const {
  // Ordered so the sidecar is byte-stable across runs.
  std::map<std::string, size_t> ordered;
  for (const auto& [address, file] : addressToFile_) {
    ordered.emplace(fmt::format("{:08X}", address), file);
  }

  nlohmann::json doc;
  doc["version"] = kPartitionVersion;
  doc["file_count"] = fileCount_;
  doc["max_file_bytes"] = maxFileBytes_;
  doc["assignments"] = ordered;
  return doc.dump(2) + "\n";
}

std::vector<std::vector<size_t>> OutputPartition::Assign(std::span<const FunctionSize> entries,
                                                         size_t maxFileBytes) {
  if (fileCount_ == 0 || maxFileBytes != maxFileBytes_) {
    if (fileCount_ != 0) {
      REXLOG_DEBUG(codegen, "Byte budget changed ({} -> {}), repartitioning from scratch",
                   maxFileBytes_, maxFileBytes);
      addressToFile_.clear();
    }
    fileCount_ = ChooseFileCount(entries, maxFileBytes);
    REXLOG_DEBUG(codegen, "Partitioning {} functions across {} output file(s)", entries.size(),
                 fileCount_);
  }
  maxFileBytes_ = maxFileBytes;

  std::vector<std::vector<size_t>> buckets(fileCount_);
  std::vector<size_t> bucketBytes(fileCount_, 0);

  std::vector<bool> placed(entries.size(), false);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto it = addressToFile_.find(entries[i].address);
    if (it == addressToFile_.end() || it->second >= fileCount_)
      continue;
    buckets[it->second].push_back(i);
    bucketBytes[it->second] += entries[i].bytes;
    placed[i] = true;
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    if (placed[i])
      continue;
    size_t target = static_cast<size_t>(std::min_element(bucketBytes.begin(), bucketBytes.end()) -
                                        bucketBytes.begin());
    buckets[target].push_back(i);
    bucketBytes[target] += entries[i].bytes;
  }

  for (auto& bucket : buckets) {
    std::sort(bucket.begin(), bucket.end(),
              [&](size_t lhs, size_t rhs) { return entries[lhs].address < entries[rhs].address; });
  }

  // Rebuilt, not updated, so functions that disappeared leave the sidecar.
  addressToFile_.clear();
  for (size_t b = 0; b < buckets.size(); ++b) {
    for (size_t entry : buckets[b]) {
      addressToFile_[entries[entry].address] = b;
    }
  }
  return buckets;
}

}  // namespace rex::codegen
