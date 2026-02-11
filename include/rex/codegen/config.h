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

#include <rex/codegen/function_graph.h>  // For JumpTable
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <filesystem>

namespace rex::codegen {

struct MidAsmHook
{
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
struct FunctionConfig
{
    uint32_t size = 0;          // Explicit size in bytes (mutually exclusive with end)
    uint32_t end = 0;           // End address, exclusive (mutually exclusive with size)
    std::string name;           // Custom symbol name (empty = auto-generate sub_XXXXXXXX)
    uint32_t parent = 0;        // Parent function address (0 = standalone, non-zero = chunk)

    // Get effective size (prefers size over end)
    uint32_t getSize(uint32_t address) const {
        return size ? size : (end > address ? end - address : 0);
    }
    // Returns true if this is a discontinuous chunk belonging to a parent function
    bool isChunk() const { return parent != 0; }
};

// Section info for analysis output
struct SectionInfo
{
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    std::string flags;  // "rx", "rw", "r" etc.
};

// Function entry for analysis output
struct FunctionEntry
{
    uint64_t address = 0;
    uint64_t size = 0;
    std::string name;  // optional, defaults to "sub_XXXXXXXX"
};

struct RecompilerConfig
{
    // === Required user-provided fields ===
    std::string projectName = "rex";     ///< Project name for output files
    std::string filePath;                ///< Path to XEX/ELF file
    std::string outDirectoryPath;        ///< Output directory for generated code

    // Patch file paths (TODO: implement patching workflow)
    std::string patchFilePath;
    std::string patchedFilePath;

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
    uint32_t maxJumpExtension = 65536;     ///< Max bytes to extend function for jump table targets
    uint32_t dataRegionThreshold = 16;     ///< Consecutive invalid instructions to mark as data region
    uint32_t largeFunctionThreshold = 1048576;  ///< 1MB - warn if function exceeds this size

    // === Manual overrides ===
    std::unordered_map<uint32_t, FunctionConfig> functions;  ///< Function/chunk configuration
    std::unordered_map<uint32_t, JumpTable> switchTables;
    std::unordered_map<uint32_t, MidAsmHook> midAsmHooks;
    uint32_t longJmpAddress = 0;
    uint32_t setJmpAddress = 0;

    // === User hints (merged with analysis results in AnalysisState) ===
    std::unordered_map<uint32_t, uint32_t> invalidInstructionHints;  ///< addr -> size
    std::unordered_set<uint32_t> knownIndirectCallHints;  ///< bctr addresses that are vtable/computed calls
    std::vector<uint32_t> exceptionHandlerFuncHints;  ///< Additional exception handler addresses

    /**
     * Load configuration from a TOML file.
     * @param configFilePath Path to the TOML config file
     */
    void Load(const std::string_view& configFilePath);

    /// Validation result containing warnings and errors.
    struct ValidationResult {
        bool valid = true;                       ///< true if no errors (warnings OK)
        std::vector<std::string> warnings;       ///< Non-fatal issues
        std::vector<std::string> errors;         ///< Fatal issues that block codegen

        explicit operator bool() const { return valid; }
    };

    /**
     * Validate the loaded configuration.
     * Checks address alignment, required fields, and sanity constraints.
     * @return ValidationResult with warnings and errors
     */
    ValidationResult Validate() const;
};

} // namespace rex::codegen
