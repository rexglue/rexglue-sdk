/**
 * @file        codegen/codegen_context.h
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
#include <rex/codegen/limits.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::codegen {

class DecodedBinary;
struct SectionInfo;
struct FunctionEntry;
struct FunctionConfig;

/**
 * Analysis state holding binary derived data and analysis results.
 * Populated during analysis and not mutated after. Separate from the user provided config.
 */
struct AnalysisState {
  std::string format; /**< "xex" or "elf" */
  /** Guest address the image is loaded at, copied from the BinaryView base address. */
  u64 loadAddress = 0;
  /** Guest address of the image entry point, which the writer names xstart. */
  u64 entryPoint = 0;
  u64 imageSize = 0; /**< Image size in bytes, copied from the BinaryView. */

  std::vector<SectionInfo> sections;            /**< No code reads or writes it. */
  std::vector<FunctionEntry> analyzedFunctions; /**< No code reads or writes it. */
  /** Entry addresses of the chunks of each parent function, keyed by parent address, from the
   * `parent` field of the function configs. */
  std::unordered_map<u32, std::vector<u32>> chunksByParent;

  /** ABI helper addresses found by analysis, 0 when not found. */
  u32 restGpr14Address = 0;
  /** Address of the first `__savegprlr_14` helper, 0 when not found. */
  u32 saveGpr14Address = 0;
  /** Address of the first `__restfpr_14` helper, 0 when not found. */
  u32 restFpr14Address = 0;
  /** Address of the first `__savefpr_14` helper, 0 when not found. */
  u32 saveFpr14Address = 0;
  /** Address of the first `__restvmx_14` helper, 0 when not found. */
  u32 restVmx14Address = 0;
  /** Address of the first `__savevmx_14` helper, 0 when not found. */
  u32 saveVmx14Address = 0;
  /** Address of the first `__restvmx_64` helper, 0 when not found. */
  u32 restVmx64Address = 0;
  /** Address of the first `__savevmx_64` helper, 0 when not found. */
  u32 saveVmx64Address = 0;

  std::unordered_map<u32, u32> invalidInstructions; /**< Maps address to size */
  std::unordered_set<u32> knownIndirectCalls;       /**< bctr addresses */
  /** Entry addresses of exception handler functions, copied from `analysis.exception_handler_funcs`
   * in the TOML. */
  std::vector<u32> exceptionHandlerFuncs;
  /** Entry addresses found in SEH scope tables, queued as functions after the .pdata functions. */
  std::vector<u32> ehDiscoveredFuncs;
};

/**
 * Unified context for the codegen pipeline.
 *
 * Owns the core data structures used by analysis and code generation.
 * Single source of truth for
 * - Binary data (BinaryView)
 * - Function graph (all functions including imports)
 * - Configuration
 * - Analysis errors
 * - Scan artifacts
 */
class CodegenContext : public rex::MoveOnly {
 public:
  /**
   * Creates a CodegenContext from a config file path and Runtime.
   * Loads the configuration from the TOML file, loads the XEX through Runtime and builds the
   * BinaryView from the loaded module.
   *
   * @param configPath Path to the TOML config file
   * @param runtime Runtime instance set up with the correct content_root
   * @return CodegenContext on success, error on failure
   */
  [[nodiscard]] static Result<CodegenContext> Create(const std::filesystem::path& configPath,
                                                     Runtime& runtime);

  /**
   * Creates a CodegenContext from a pre-loaded binary and config.
   * Used by tests that load the binary differently.
   *
   * @param binary Pre-loaded BinaryView, moved into the context
   * @param config Pre-loaded RecompilerConfig, moved into the context
   * @return CodegenContext owning both
   */
  static CodegenContext Create(BinaryView binary, RecompilerConfig config);

  /**
   * Takes ownership of the state of another context.
   *
   * @param other Context to take from, left empty.
   */
  CodegenContext(CodegenContext&&) noexcept;
  /**
   * Releases the owned state, then takes that of another context.
   *
   * @param other Context to take from, left empty.
   * @return This context
   */
  CodegenContext& operator=(CodegenContext&&) noexcept;
  /** Destroys the context and the decoded binary it owns. */
  ~CodegenContext();

  FunctionGraph graph; /**< All functions including imports */
  /** Errors the analysis phases collected, which stop the writer unless forced. */
  AnalysisErrors errors;

  /** Scan phase artifacts passed to Discover for scanner setup. */
  struct {
    std::vector<CodeRegion> codeRegions; /**< Null delimited code regions */
    /** Address ranges `[first, second)` of runs of at least `dataRegionThreshold` words equal to 0
     * or 0xFFFFFFFF. */
    std::vector<std::pair<u32, u32>> dataRegions;
    std::unordered_map<u32, u32> pdataSizes; /**< Maps address to size */
  } scan;

  const BinaryView& binary() const { return binary_; }
  BinaryView& binary() { return binary_; }

  /**
   * Returns the decoded binary. Valid only after initDecoded().
   *
   * @return Decoded binary owned by the context
   */
  DecodedBinary& decoded();
  /**
   * Returns the decoded binary. Valid only after initDecoded().
   *
   * @return Decoded binary owned by the context
   */
  const DecodedBinary& decoded() const;

  /** Creates the DecodedBinary once the context is at its final location. Runs once after
   * Create() and before decoded(). */
  void initDecoded();

  bool hasDecoded() const { return decoded_ != nullptr; }

  RecompilerConfig& Config() { return config_; }
  const RecompilerConfig& Config() const { return config_; }

  AnalysisState& analysisState() { return analysisState_; }
  const AnalysisState& analysisState() const { return analysisState_; }

  /** Analysis limits for every phase run against this context. */
  const Limits& limits() const { return limits_; }
  void setLimits(const Limits& limits) { limits_ = limits; }

  const std::filesystem::path& configDir() const { return configDir_; }
  void setConfigDir(const std::filesystem::path& dir) { configDir_ = dir; }

  void setDllModule(bool is_dll) { is_dll_module_ = is_dll; }
  bool isDllModule() const { return is_dll_module_; }

  void setHasDllModules(bool has) { has_dll_modules_ = has; }
  bool hasDllModules() const { return has_dll_modules_; }

 private:
  CodegenContext() = default;

  BinaryView binary_;
  RecompilerConfig config_;
  AnalysisState analysisState_;
  Limits limits_;
  std::unique_ptr<DecodedBinary> decoded_; /**< Created by initDecoded() */
  std::filesystem::path configDir_;
  bool is_dll_module_ = false;
  bool has_dll_modules_ = false;
};

}  // namespace rex::codegen
