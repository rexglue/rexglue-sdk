/**
 * @file        rexcodegen/internal/config.h
 * @brief       Recompiler configuration types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/codegen/function_graph.h>  // For JumpTable

namespace rex::codegen {

struct MidAsmHook {
  std::string name;
  std::vector<std::string> registers;

  bool ret = false;
  bool returnOnTrue = false;
  bool returnOnFalse = false;

  uint32_t jumpAddress = 0;
  uint32_t jumpAddressOnTrue = 0;
  uint32_t jumpAddressOnFalse = 0;

  bool afterInstruction = false;
};

// Unified function/chunk configuration
// A "chunk" is simply a function entry with a non-zero parent field
struct FunctionConfig {
  uint32_t size = 0;    // Explicit size in bytes (mutually exclusive with end)
  uint32_t end = 0;     // End address, exclusive (mutually exclusive with size)
  std::string name;     // Custom symbol name (empty = auto-generate sub_XXXXXXXX)
  uint32_t parent = 0;  // Parent function address (0 = standalone, non-zero = chunk)

  // Get effective size (prefers size over end)
  uint32_t getSize(uint32_t address) const {
    return size ? size : (end > address ? end - address : 0);
  }
  // Returns true if this is a discontinuous chunk belonging to a parent function
  bool isChunk() const { return parent != 0; }
};

// Section info for analysis output
struct SectionInfo {
  std::string name;
  uint64_t address = 0;
  uint64_t size = 0;
  std::string flags;  // "rx", "rw", "r" etc.
};

// Function entry for analysis output
struct FunctionEntry {
  uint64_t address = 0;
  uint64_t size = 0;
  std::string name;  // optional, defaults to "sub_XXXXXXXX"
};

struct RecompilerConfig {
  // === Required user-provided fields ===
  std::string projectName = "rex";  ///< Project name for output files
  std::string filePath;             ///< Path to XEX/ELF file
  std::string outDirectoryPath;     ///< Output directory for generated code
  std::string templateDir;          ///< Optional custom template directory for overrides

  // Patch file paths (TODO: implement patching workflow)
  std::string patchFilePath;
  std::string patchedFilePath;

  /// Optional path to a TOML carrying a `[[function]]` array of
  /// (address, display) entries used to override the default
  /// sub_XXXXXXXX symbol names emitted by codegen. Resolved at
  /// Load() time: relative paths bind to the directory of the file
  /// that introduced them (same semantics as `includes`); absolute
  /// paths and CLI overrides pass through unchanged. Empty when no
  /// mapping is configured (rename pass becomes a no-op).
  std::string mappingFilePath;

  // === Code generation options (optional) ===
  bool skipLr = false;
  bool ctrAsLocalVariable = false;
  bool xerAsLocalVariable = false;
  bool reservedRegisterAsLocalVariable = false;
  bool skipMsr = false;
  bool crRegistersAsLocalVariables = false;
  bool nonArgumentRegistersAsLocalVariables = false;
  bool nonVolatileRegistersAsLocalVariables = false;
  bool generateExceptionHandlers = false;  ///< Generate SEH exception handler wrappers

  // === Analysis tuning (optional) ===
  uint32_t maxJumpExtension = 65536;  ///< Max bytes to extend function for jump table targets
  uint32_t dataRegionThreshold = 16;  ///< Consecutive invalid instructions to mark as data region
  uint32_t largeFunctionThreshold = 1048576;  ///< 1MB - warn if function exceeds this size

  // === Manual overrides ===
  std::unordered_map<uint32_t, FunctionConfig> functions;  ///< Function/chunk configuration
  std::unordered_map<uint32_t, JumpTable> switchTables;
  std::unordered_map<uint32_t, MidAsmHook> midAsmHooks;

  /// Address -> sanitized C++ identifier sourced from the
  /// `[[function]]` array of `mappingFilePath`. Populated by
  /// LoadMappings(); empty when no mapping is loaded. Consumed by
  /// ApplyMappingNames() (see codegen.cpp) which overrides the
  /// auto-generated sub_XXXXXXXX names on FunctionNodes after
  /// Analyze. Addresses present here but not corresponding to a
  /// function entry in the graph (e.g. labels inside a parent
  /// function under the chunks-as-labels model) are silently
  /// skipped at apply time.
  std::unordered_map<uint32_t, std::string> functionNames;
  uint32_t longJmpAddress = 0;
  uint32_t setJmpAddress = 0;

  // === rexcrt: CRT function address overrides ===
  // Maps function name -> guest address (e.g. "CreateFileA" -> 0x8248B780)
  // Parsed from [rexcrt] TOML table. Codegen generates rexcrt_<Name> entries.
  std::unordered_map<std::string, uint32_t> rexcrtFunctions;

  // === User hints (merged with analysis results in AnalysisState) ===
  std::unordered_map<uint32_t, uint32_t> invalidInstructionHints;  ///< addr -> size
  std::unordered_set<uint32_t>
      knownIndirectCallHints;  ///< bctr addresses that are vtable/computed calls
  std::vector<uint32_t> exceptionHandlerFuncHints;  ///< Additional exception handler addresses

  /**
   * Load configuration from a TOML file.
   *
   * Supports an optional `includes` array for layered config. Paths in
   * `includes` resolve relative to the including file's directory.  Merge
   * semantics: scalars last-wins, keyed tables additive (same key = last
   * wins), arrays-of-tables deduplicated by primary key, sets additive.
   *
   * @param configFilePath Path to the TOML config file
   * @return true on success, false on error (parse failure, circular
   *         include, depth exceeded)
   */
  bool Load(const std::string_view& configFilePath);

  /// Load `mappingFilePath` into `functionNames`.
  ///
  /// Idempotent: clears `functionNames` first so a CLI override can
  /// rebind the path and replace (not merge) any prior contents.
  /// No-op when `mappingFilePath` is empty. The path must be
  /// absolute by the time this is called -- Load() resolves any
  /// relative path against the introducing config file's
  /// directory; CLI overrides should pre-resolve against cwd.
  ///
  /// Reads the pdb-toml `[[function]]` schema. Per entry only
  /// `address` (uint32) and `display` (string) are consumed; all
  /// other fields are ignored. Each `display` is sanitized into a
  /// C++ identifier and suffixed with `_<UPPER_HEX_ADDR>` so the
  /// names are trivially collision-free and round-trippable to the
  /// source PDB.
  ///
  /// @return true on success (including the "no mapping" no-op),
  ///         false on parse failure or schema mismatch.
  bool LoadMappings();

  /// Validation result containing warnings and errors.
  struct ValidationResult {
    bool valid = true;                  ///< true if no errors (warnings OK)
    std::vector<std::string> warnings;  ///< Non-fatal issues
    std::vector<std::string> errors;    ///< Fatal issues that block codegen

    explicit operator bool() const { return valid; }
  };

  /**
   * Validate the loaded configuration.
   * Checks address alignment, required fields, and sanity constraints.
   * @return ValidationResult with warnings and errors
   */
  ValidationResult Validate() const;
};

}  // namespace rex::codegen
