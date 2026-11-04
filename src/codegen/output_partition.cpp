#include "output_partition.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <system_error>

#include <format>
#include <nlohmann/json.hpp>

#include <rex/logging/log.h>
#include "codegen_categories.h"

namespace rex::codegen {

namespace {

constexpr int kPartitionVersion = 2;

size_t ChooseFileCount(std::span<const FunctionSize> entries, size_t max_file_bytes) {
  if (max_file_bytes == 0)
    return 1;
  size_t total = 0;
  for (const auto& entry : entries) {
    total += entry.bytes;
  }
  return std::max<size_t>(1, (total + max_file_bytes - 1) / max_file_bytes);
}

}  // namespace

OutputPartition OutputPartition::Load(const std::filesystem::path& path) {
  OutputPartition partition;

  std::ifstream in(path, std::ios::binary);
  if (!in)
    return partition;

  nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    REXCG_WARN("Ignoring malformed partition sidecar: {}", path.string());
    return partition;
  }

  auto version = doc.find("version");
  if (version == doc.end() || !version->is_number_integer() ||
      version->get<int>() != kPartitionVersion) {
    REXCG_DEBUG("Ignoring partition sidecar from another version: {}", path.string());
    return partition;
  }

  if (auto it = doc.find("file_count"); it != doc.end() && it->is_number_unsigned())
    partition.file_count_ = it->get<size_t>();
  if (auto it = doc.find("max_file_bytes"); it != doc.end() && it->is_number_unsigned())
    partition.max_file_bytes_ = it->get<size_t>();

  auto it = doc.find("assignments");
  if (it == doc.end() || !it->is_object())
    return partition;

  for (const auto& [key, value] : it->items()) {
    if (!value.is_number_unsigned())
      continue;
    u32 address = 0;
    const char* first = key.data();
    const char* last = first + key.size();
    auto [ptr, ec] = std::from_chars(first, last, address, 16);
    if (ec != std::errc{} || ptr != last)
      continue;
    partition.address_to_file_[address] = value.get<size_t>();
  }
  return partition;
}

std::string OutputPartition::Serialize() const {
  // Ordered so the sidecar bytes are stable across runs.
  std::map<std::string, size_t> ordered;
  for (const auto& [address, file] : address_to_file_) {
    ordered.emplace(std::format("{:08X}", address), file);
  }

  nlohmann::json doc;
  doc["version"] = kPartitionVersion;
  doc["file_count"] = file_count_;
  doc["max_file_bytes"] = max_file_bytes_;
  doc["assignments"] = ordered;
  return doc.dump(2) + "\n";
}

std::vector<std::vector<size_t>> OutputPartition::Assign(std::span<const FunctionSize> entries,
                                                         size_t max_file_bytes) {
  if (file_count_ == 0 || max_file_bytes != max_file_bytes_) {
    if (file_count_ != 0) {
      REXCG_DEBUG("Byte budget changed ({} -> {}), repartitioning from scratch", max_file_bytes_,
                  max_file_bytes);
      address_to_file_.clear();
    }
    file_count_ = ChooseFileCount(entries, max_file_bytes);
    REXCG_DEBUG("Partitioning {} functions across {} output file(s)", entries.size(), file_count_);
  }
  max_file_bytes_ = max_file_bytes;

  std::vector<std::vector<size_t>> buckets(file_count_);
  std::vector<size_t> bucket_bytes(file_count_, 0);

  std::vector<bool> placed(entries.size(), false);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto it = address_to_file_.find(entries[i].address);
    if (it == address_to_file_.end() || it->second >= file_count_)
      continue;
    buckets[it->second].push_back(i);
    bucket_bytes[it->second] += entries[i].bytes;
    placed[i] = true;
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    if (placed[i])
      continue;
    size_t target = static_cast<size_t>(std::min_element(bucket_bytes.begin(), bucket_bytes.end()) -
                                        bucket_bytes.begin());
    buckets[target].push_back(i);
    bucket_bytes[target] += entries[i].bytes;
  }

  for (auto& bucket : buckets) {
    std::sort(bucket.begin(), bucket.end(),
              [&](size_t lhs, size_t rhs) { return entries[lhs].address < entries[rhs].address; });
  }

  // Rebuilt, not updated, so functions that disappeared leave the sidecar.
  address_to_file_.clear();
  for (size_t b = 0; b < buckets.size(); ++b) {
    for (size_t entry : buckets[b]) {
      address_to_file_[entries[entry].address] = b;
    }
  }
  return buckets;
}

}  // namespace rex::codegen
