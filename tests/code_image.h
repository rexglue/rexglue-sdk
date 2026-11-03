/**
 * @file        tests/code_image.h
 * @brief       Builds a single module code image and runs code generation over it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/result.h>
#include <rex/types.h>

namespace rex::log {
class ISink;
}  // namespace rex::log

namespace rex::test {

struct TempFolder;
struct XEXImportSpec;

inline constexpr u32 kCodeImageBase = 0x82000000;
inline constexpr u32 kCodeImageSize = 0x10000;
inline constexpr u32 kCodeImageCodeOffset = 0x1000;
inline constexpr u32 kCodeImagePDataOffset = 0x2000;
inline constexpr u32 kCodeImageRDataOffset = 0x3000;
inline constexpr u32 kPDataSaveMillicode = 0u << 30;
inline constexpr u32 kPDataNoHandler = 1u << 30;
inline constexpr u32 kPDataRestoreMillicode = 2u << 30;
inline constexpr u32 kPDataHandler = 3u << 30;

struct PDataEntry {
  u32 begin;                   /**< Address of the first instruction. */
  u32 length;                  /**< Instruction count including the prologue. */
  u32 prolog = 0;              /**< Instruction count of the prologue. */
  u32 flags = kPDataNoHandler; /**< Flag bits of the entry. */
};

/**
 * Builds a PE image with a code section and an exception directory.
 *
 * @param words The instruction words of the code section.
 * @param pdata The exception directory entries, at least 1.
 * @param rdata The words of a read only data section, none to omit it.
 * @return The image bytes.
 */
std::vector<u8> BuildCodeImage(std::span<const u32> words, std::span<const PDataEntry> pdata,
                               std::span<const u32> rdata = {});

/** Returns the contents of every file under a folder, concatenated. */
std::string ReadGeneratedTree(const std::filesystem::path& root);

/** Runs code generation over an image with no imports and no log sink. */
Result<void> GenerateFromImage(const TempFolder& folder, std::vector<u8> image,
                               std::string_view extra_manifest = {}, bool force = true);

/**
 * Runs code generation over an image as the entrypoint of a project.
 *
 * @param folder The project folder, which receives the output.
 * @param image The image bytes.
 * @param extra_manifest Text appended to the manifest.
 * @param force True to continue past validation errors.
 * @param imports The import libraries of the image and the thunk addresses of each.
 * @param sink Receives the log messages of the run, or null for none.
 * @param project_keys Lines added to the [project] table.
 * @return The result of the run, with the output under folder/generated/entry.
 */
Result<void> GenerateFromImage(const TempFolder& folder, std::vector<u8> image,
                               std::string_view extra_manifest, bool force,
                               std::vector<XEXImportSpec> imports, log::ISink* sink = nullptr,
                               std::string_view project_keys = {});

}  // namespace rex::test
