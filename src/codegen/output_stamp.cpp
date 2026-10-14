#include <rex/codegen/output_stamp.h>

#include <fstream>

#include <format>
#include <nlohmann/json.hpp>

#include <rex/hash.h>

#include <rex/logging.h>
#include "file_io.h"

namespace rex::codegen {

/** Layout version of the stamp. */
constexpr int kStampVersion = 1;

std::optional<OutputStamp> OutputStamp::Load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;

  nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    REXLOG_WARN(codegen, "Ignoring malformed codegen stamp: {}", path.string());
    return std::nullopt;
  }

  auto version = doc.find("version");
  if (version == doc.end() || !version->is_number_integer() || version->get<int>() != kStampVersion)
    return std::nullopt;

  OutputStamp stamp;
  if (auto it = doc.find("fingerprint"); it != doc.end() && it->is_string())
    stamp.fingerprint = it->get<std::string>();
  if (stamp.fingerprint.empty())
    return std::nullopt;

  if (auto it = doc.find("outputs"); it != doc.end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (entry.is_string())
        stamp.outputs.push_back(entry.get<std::string>());
    }
  }
  return stamp;
}

std::string OutputStamp::Serialize() const {
  nlohmann::json doc;
  doc["version"] = kStampVersion;
  doc["fingerprint"] = fingerprint;
  doc["outputs"] = outputs;
  return doc.dump(2) + "\n";
}

std::string ComputeInputFingerprint(std::span<const std::filesystem::path> input_files,
                                    std::string_view sdk_version,
                                    std::span<const std::string> flag_values) {
  std::string accumulator;
  accumulator += "sdk=";
  accumulator += sdk_version;
  accumulator += '\n';

  for (const auto& flag : flag_values) {
    accumulator += "flag=";
    accumulator += flag;
    accumulator += '\n';
  }

  for (const auto& path : input_files) {
    // Hash content, not mtime, so the stamp survives a checkout, a copy or a touch.
    std::error_code ec;
    std::string digest = "<missing>";
    if (std::filesystem::exists(path, ec)) {
      digest = rex::HashFile(path);
      if (digest.empty()) {
        // A constant would report a locked input as unchanged and skip the module forever.
        REXLOG_WARN(codegen, "Could not read {} for fingerprinting", path.string());
        digest = std::format("<unreadable:{}:{}>", std::filesystem::file_size(path, ec),
                             std::filesystem::last_write_time(path, ec).time_since_epoch().count());
      }
    }
    accumulator += std::format("file={} {}\n", path.filename().string(), digest);
  }

  return rex::HashBytes(accumulator);
}

namespace {

std::string EscapeDepfilePath(const std::filesystem::path& path) {
  std::string source = std::filesystem::absolute(path).generic_string();
  std::string escaped;
  escaped.reserve(source.size() + 8);
  for (char c : source) {
    if (c == ' ' || c == '#')
      escaped += '\\';
    if (c == '$')
      escaped += '$';
    escaped += c;
  }
  return escaped;
}

}  // namespace

bool WriteDepfile(const std::filesystem::path& path, const std::filesystem::path& target,
                  std::span<const std::filesystem::path> inputs) {
  std::string out = EscapeDepfilePath(target);
  out += ':';
  for (const auto& input : inputs) {
    out += " \\\n  ";
    out += EscapeDepfilePath(input);
  }
  out += '\n';

  return WriteFileBytes(path, out);
}

bool AreOutputsUpToDate(const OutputStamp& stamp, std::string_view fingerprint,
                        const std::filesystem::path& output_dir) {
  if (stamp.fingerprint != fingerprint)
    return false;
  if (stamp.outputs.empty())
    return false;

  for (const auto& name : stamp.outputs) {
    std::error_code ec;
    if (!std::filesystem::exists(output_dir / name, ec))
      return false;
  }
  return true;
}

}  // namespace rex::codegen
