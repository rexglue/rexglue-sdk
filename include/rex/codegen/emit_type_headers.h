/**
 * @file        rex/codegen/emit_type_headers.h
 * @brief       Type / enum header emission from RecompilerConfig
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

namespace rex::codegen {

class CodegenContext;

/**
 * Emit `<outDir>/mappings_generated/{types,enums}.h` from
 * `cfg.types` / `cfg.enums`.
 *
 * No-op when both vectors are empty (the default for projects that
 * do not configure `[[types]]` / `[[enums]]`).
 *
 * Header layout: each `RecompilerType` becomes an opaque
 * `struct <name> { unsigned char _opaque[size]; };` plus a sibling
 * `<name>_offsets` namespace of `inline constexpr size_t` field
 * offsets and a `static_assert(sizeof(<name>) == size)` drift check.
 * Each `RecompilerEnum` becomes an `enum class <name> :
 * <fixed_width_underlying>` with sanitized enumerators. See
 * `docs/types_and_enums.md` for the full schema and rationale.
 *
 * Output directory is `configDir / cfg.outDirectoryPath /
 * "mappings_generated"`. Created if missing; emission errors are
 * logged but do not abort codegen.
 */
void EmitTypeHeaders(const CodegenContext& ctx);

}  // namespace rex::codegen
