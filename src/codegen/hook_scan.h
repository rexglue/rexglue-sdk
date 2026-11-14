/**
 * @file        codegen/hook_scan.h
 * @brief       Reads mid asm hooks declared in C++ sources
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
#include <vector>

#include "config.h"
#include <rex/ppc/midasm.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::codegen {

/** What a mid asm body does with the generated code around the call. */
using MidAsmType = rex::ppc::detail::MidAsmType;

/** One REX_MIDASM_AT line with the body it attaches. */
struct SourceMidAsmHook {
  std::string name; /**< The body name, which is its linked name. */
  u32 address;
  bool after;                          /**< True to run after the instruction. */
  u32 target;                          /**< The branch target, 0 for a body that cannot branch. */
  MidAsmType type;                     /**< What the body does with the code around it. */
  std::vector<std::string> registers;  /**< The register tokens in parameter order. */
  std::vector<std::string> parameters; /**< The parameter types of the body in order. */
  std::filesystem::path file;
  u32 line; /**< The 1 based line of the macro. */
};

/**
 * Reads every REX_MIDASM_AT line under the given folders. A body macro and its attachments may sit
 * in different files. Register tokens are checked with the same parser the macros use. A macro
 * inside a preprocessor conditional is refused, because the scan cannot know which branch builds.
 *
 * @param roots The folders to scan, searched recursively for .cpp, .cc, .h and .hpp files.
 * @param scanned_files Receives every folder searched and every file read, in the order read.
 * @return The attachments sorted by address, or an error naming the file and line of the first
 *     macro that cannot be read.
 */
[[nodiscard]] Result<std::vector<SourceMidAsmHook>> ScanHookSources(
    std::span<const std::filesystem::path> roots,
    std::vector<std::filesystem::path>& scanned_files);

/** Converts an attachment to the hook entry the generated code is built from. */
MidAsmHook ToMidAsmHook(const SourceMidAsmHook& hook);

}  // namespace rex::codegen
