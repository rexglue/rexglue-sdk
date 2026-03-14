/**
 * @file        rexcodegen/codegen_context.h
 * @brief       Unified context for codegen pipeline
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_region.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/result.h>

namespace rex {
class Runtime;

namespace runtime {
class ExportResolver;
}
}  // namespace rex

namespace rex::codegen {

// Forward declarations
class DecodedBinary;
struct SectionInfo;
struct FunctionEntry;
struct FunctionConfig;

/**
 * Analysis state holding binary-derived data and analysis results.
 * This data is populated during analysis and should not be mutated after.
 * Separates analysis state from user-provided config.
 */
struct AnalysisState {
  // Binary-derived (set once from BinaryView)
  std::string format;        ///< "xex" or "elf"
  uint64_t loadAddress = 0;  ///< Image base address
  uint64_t entryPoint = 0;   ///< Entry point address
  uint64_t imageSize = 0;    ///< Total image size

  // Analysis results
  std::vector<SectionInfo> sections;                                   ///< Sections from binary
  std::vector<FunctionEntry> analyzedFunctions;                        ///< Discovered functions
  std::unordered_map<uint32_t, std::vector<uint32_t>> chunksByParent;  ///< Chunk lookup

  // Auto-detected ABI helpers (0 = not found)
  uint32_t restGpr14Address = 0;
  uint32_t saveGpr14Address = 0;
  uint32_t restFpr14Address = 0;
  uint32_t saveFpr14Address = 0;
  uint32_t restVmx14Address = 0;
  uint32_t saveVmx14Address = 0;
  uint32_t restVmx64Address = 0;
  uint32_t saveVmx64Address = 0;

  // Merged results (user hints + analysis-detected)
  std::unordered_map<uint32_t, uint32_t> invalidInstructions;  ///< addr -> size
  std::unordered_set<uint32_t> knownIndirectCalls;             ///< bctr addresses
  std::vector<uint32_t> exceptionHandlerFuncs;                 ///< Handler addresses
  std::vector<uint32_t> ehDiscoveredFuncs;                     ///< EH-discovered function addresses
};

/**
 * Unified context for the entire codegen pipeline.
 *
 * This class owns all the core data structures used throughout analysis
 * and code generation. It replaces the previous scattered ownership where
 * Recompiler owned some data and AnalysisContext owned other data.
 *
 * Single source of truth for:
 * - Binary data (BinaryView)
 * - Function graph (all functions including imports)
 * - Configuration
 * - Analysis errors
 * - Scan artifacts
 */
class CodegenContext {
 public:
  // === FACTORY ===
  /**
   * Create a CodegenContext from config file path and Runtime.
   *
   * This is the primary way to create a context. It:
   * 1. Loads configuration from the TOML file
   * 2. Loads the XEX via Runtime
   * 3. Creates BinaryView from the loaded module
   *
   * @param configPath Path to the TOML config file
   * @param runtime Runtime instance (must be set up with correct content_root)
   * @return CodegenContext on success, error on failure
   */
  static Result<CodegenContext> Create(const std::filesystem::path& configPath, Runtime& runtime);

  /**
   * Create a CodegenContext from pre-loaded binary and config.
   * Primarily for testing where binary is loaded differently.
   *
   * @param binary Pre-loaded BinaryView (moved into context)
   * @param config Pre-loaded RecompilerConfig (moved into context)
   */
  static CodegenContext Create(BinaryView binary, RecompilerConfig config);

  // Non-copyable, movable
  CodegenContext(const CodegenContext&) = delete;
  CodegenContext& operator=(const CodegenContext&) = delete;
  CodegenContext(CodegenContext&&);
  CodegenContext& operator=(CodegenContext&&);
  ~CodegenContext();

  // === OWNED DATA (single source of truth) ===
  FunctionGraph graph;    ///< All functions (including imports)
  AnalysisErrors errors;  ///< Accumulated errors

  /// Scan phase artifacts (passed to Discover for scanner setup)
  struct {
    std::vector<CodeRegion> codeRegions;  ///< Null-delimited code regions
    std::vector<std::pair<uint32_t, uint32_t>> dataRegions;
    std::unordered_map<uint32_t, uint32_t> pdataSizes;  // address -> size
  } scan;

  // === ACCESSORS ===
  const BinaryView& binary() const { return binary_; }
  BinaryView& binary() { return binary_; }

  /// Access the decoded binary (must call initDecoded() first)
  DecodedBinary& decoded();
  const DecodedBinary& decoded() const;

  /// Initialize DecodedBinary after context is in final location
  /// Call this once after Create() before accessing decoded()
  void initDecoded();

  bool hasDecoded() const { return decoded_ != nullptr; }

  RecompilerConfig& Config() { return config_; }
  const RecompilerConfig& Config() const { return config_; }

  AnalysisState& analysisState() { return analysisState_; }
  const AnalysisState& analysisState() const { return analysisState_; }

  runtime::ExportResolver* resolver() const { return resolver_; }

  const std::filesystem::path& configDir() const { return configDir_; }

 private:
  CodegenContext() = default;

  BinaryView binary_;                       ///< Binary data + sections (owned)
  RecompilerConfig config_;                 ///< User configuration (owned)
  AnalysisState analysisState_;             ///< Analysis state (populated during analysis)
  std::unique_ptr<DecodedBinary> decoded_;  ///< Decoded instructions (created via initDecoded())
  runtime::ExportResolver* resolver_ = nullptr;  ///< For runtime resolution (borrowed)
  std::filesystem::path configDir_;  ///< Directory containing config file (for relative paths)
};

}  // namespace rex::codegen
