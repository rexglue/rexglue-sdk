/**
 * @file        rex/codegen/test_analyze.h
 * @brief       Test binary analysis - populates FunctionGraph from map symbols
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace rex::codegen {

class CodegenContext;

/**
 * @brief Analyze a test binary using map symbols as function entry points.
 *
 * Filters symbols to test_ prefixed entries, adds them as functions to
 * ctx.graph with single blocks, then scans for bl instructions and
 * registers call edges.
 *
 * @param ctx         Codegen context whose graph will be populated
 * @param testName    Stem name used for function naming (e.g. "addi")
 * @param symbols     Address-to-name map from parse_map_file()
 * @param baseAddress Base address the binary was linked at
 * @param data        Pointer to raw binary data
 * @param dataSize    Size of binary data in bytes
 */
void AnalyzeTestBinary(CodegenContext& ctx, std::string_view testName,
                       const std::map<size_t, std::string>& symbols, uint32_t baseAddress,
                       const uint8_t* data, size_t dataSize);

}  // namespace rex::codegen
