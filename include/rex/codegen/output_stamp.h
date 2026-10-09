/**
 * @file        codegen/output_stamp.h
 * @brief       Input fingerprinting so unchanged modules skip codegen entirely
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::codegen {

inline constexpr std::string_view kStampFileName = "codegen.stamp";
inline constexpr std::string_view kBuildStampFileName = "codegen.build.stamp";
inline constexpr std::string_view kDepfileName = "codegen.d";

/** What a module output was generated from, and what it produced. */
struct OutputStamp {
  std::string fingerprint;          /**< Hash of the inputs the outputs were generated from */
  std::vector<std::string> outputs; /**< Basenames relative to the output directory */

  /**
   * Loads a stamp.
   *
   * @param path Stamp file path
   * @return The stamp, or nullopt when missing, unreadable or malformed
   */
  static std::optional<OutputStamp> Load(const std::filesystem::path& path);

  /**
   * Serializes the stamp as JSON.
   *
   * @return JSON text ending in a newline
   */
  std::string Serialize() const;
};

/**
 * Hashes everything that can change emitted output for one module. A missing input contributes a
 * "missing" marker, so a file appearing later still changes the fingerprint.
 *
 * @param inputFiles Input files to hash
 * @param sdkVersion SDK version, stable across commits as floor plus channel such as "0.10.0-dev"
 * @param flagValues Flag values to hash
 * @return Fingerprint of the inputs
 */
std::string ComputeInputFingerprint(std::span<const std::filesystem::path> inputFiles,
                                    std::string_view sdkVersion,
                                    std::span<const std::string> flagValues);

/**
 * Reports whether a stamp matches a fingerprint and all of its outputs exist.
 *
 * @param stamp Stamp loaded from the previous run
 * @param fingerprint Fingerprint of the current inputs
 * @param outputDir Directory holding the outputs
 * @return True when the fingerprints match, the stamp lists outputs and every one exists
 */
bool OutputsAreUpToDate(const OutputStamp& stamp, std::string_view fingerprint,
                        const std::filesystem::path& outputDir);

/**
 * Writes a Make syntax dependency file. `target` must match the declared output of the
 * consuming build rule byte for byte or the rule rejects the file.
 *
 * @param path Depfile path
 * @param target Output the rule declares
 * @param inputs Input files the output depends on
 * @return True on success
 */
bool WriteDepfile(const std::filesystem::path& path, const std::filesystem::path& target,
                  std::span<const std::filesystem::path> inputs);

}  // namespace rex::codegen
