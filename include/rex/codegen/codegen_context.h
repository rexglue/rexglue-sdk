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
  AnalysisState() = default;
  AnalysisState(const AnalysisState&) = default;
  AnalysisState& operator=(const AnalysisState&) = default;
  AnalysisState(AnalysisState&&) noexcept(false) = default;
  AnalysisState& operator=(AnalysisState&&) noexcept(false) = default;
  ~AnalysisState() = default;

  std::string format; /**< "xex" or "elf" */
  /** Guest address the image is loaded at, copied from the BinaryView base address. */
  u64 load_address = 0;
  /** Guest address of the image entry point, which the writer names xstart. */
  u64 entry_point = 0;
  u64 image_size = 0; /**< Image size in bytes, copied from the BinaryView. */

  std::vector<SectionInfo> sections;             /**< No code reads or writes it. */
  std::vector<FunctionEntry> analyzed_functions; /**< No code reads or writes it. */
  /** Entry addresses of the chunks of each parent function, keyed by parent address, from the
   * `parent` field of the function configs. */
  std::unordered_map<u32, std::vector<u32>> chunks_by_parent;

  /** ABI helper addresses found by analysis, 0 when not found. */
  u32 rest_gpr14_address = 0;
  /** Address of the first `__savegprlr_14` helper, 0 when not found. */
  u32 save_gpr14_address = 0;
  /** Address of the first `__restfpr_14` helper, 0 when not found. */
  u32 rest_fpr14_address = 0;
  /** Address of the first `__savefpr_14` helper, 0 when not found. */
  u32 save_fpr14_address = 0;
  /** Address of the first `__restvmx_14` helper, 0 when not found. */
  u32 rest_vmx14_address = 0;
  /** Address of the first `__savevmx_14` helper, 0 when not found. */
  u32 save_vmx14_address = 0;
  /** Address of the first `__restvmx_64` helper, 0 when not found. */
  u32 rest_vmx64_address = 0;
  /** Address of the first `__savevmx_64` helper, 0 when not found. */
  u32 save_vmx64_address = 0;

  /** A function another user module exports by ordinal, imported through a thunk. */
  struct ModuleImportRef {
    std::string library; /**< Base name of the exporting module. */
    u16 ordinal = 0;     /**< Export ordinal. */
  };
  /** Imports of functions that a user module exports, keyed by the import thunk address. */
  std::unordered_map<u32, ModuleImportRef> module_imports;

  std::unordered_map<u32, u32> invalid_instructions; /**< Maps address to size */
  std::unordered_set<u32> known_indirect_calls;      /**< bctr addresses */
  /** Entry addresses of exception handler functions, copied from `analysis.exception_handler_funcs`
   * in the TOML. */
  std::vector<u32> exception_handler_funcs;
  /** Entry addresses found in SEH scope tables, queued as functions after the .pdata functions. */
  std::vector<u32> eh_discovered_funcs;
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
   * @param config_path Path to the TOML config file
   * @param runtime Runtime instance set up with the correct content_root
   * @return CodegenContext on success, error on failure
   */
  [[nodiscard]] static Result<CodegenContext> Create(const std::filesystem::path& config_path,
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
  CodegenContext(CodegenContext&&) noexcept(false);
  /**
   * Releases the owned state, then takes that of another context.
   *
   * @param other Context to take from, left empty.
   * @return This context
   */
  CodegenContext& operator=(CodegenContext&&) noexcept(false);
  /** Destroys the context and the decoded binary it owns. */
  ~CodegenContext();

  FunctionGraph graph; /**< All functions including imports */
  /** Errors the analysis phases collected, which stop the writer unless forced. */
  AnalysisErrors errors;

  /** Scan phase artifacts passed to Discover for scanner setup. */
  struct ScanArtifacts {
    ScanArtifacts() = default;
    ScanArtifacts(const ScanArtifacts&) = default;
    ScanArtifacts& operator=(const ScanArtifacts&) = default;
    ScanArtifacts(ScanArtifacts&&) noexcept(false) = default;
    ScanArtifacts& operator=(ScanArtifacts&&) noexcept(false) = default;
    ~ScanArtifacts() = default;

    std::vector<CodeRegion> code_regions; /**< Null delimited code regions */
    /** Address ranges `[first, second)` of runs of at least `data_region_threshold` words equal to
     * 0 or 0xFFFFFFFF. */
    std::vector<std::pair<u32, u32>> data_regions;
    std::unordered_map<u32, u32> pdata_sizes; /**< Maps address to size */
  } scan;

  const BinaryView& GetBinary() const { return binary_; }
  BinaryView& GetBinary() { return binary_; }

  /**
   * Returns the decoded binary. Valid only after InitDecoded().
   *
   * @return Decoded binary owned by the context
   */
  DecodedBinary& GetDecoded();
  /**
   * Returns the decoded binary. Valid only after InitDecoded().
   *
   * @return Decoded binary owned by the context
   */
  const DecodedBinary& GetDecoded() const;

  /** Creates the DecodedBinary once the context is at its final location. Runs once after
   * Create() and before GetDecoded(). */
  void InitDecoded();

  bool HasDecoded() const { return decoded_ != nullptr; }

  RecompilerConfig& GetConfig() { return config_; }
  const RecompilerConfig& GetConfig() const { return config_; }

  AnalysisState& GetAnalysisState() { return analysis_state_; }
  const AnalysisState& GetAnalysisState() const { return analysis_state_; }

  /** Analysis limits for every phase run against this context. */
  const Limits& GetLimits() const { return limits_; }
  void SetLimits(const Limits& limits) { limits_ = limits; }

  const std::filesystem::path& GetConfigDir() const { return config_dir_; }
  void SetConfigDir(const std::filesystem::path& dir) { config_dir_ = dir; }

  void SetDLLModule(bool is_dll) { is_dll_module_ = is_dll; }
  bool IsDLLModule() const { return is_dll_module_; }

  void SetHasDLLModules(bool has) { has_dll_modules_ = has; }
  bool HasDLLModules() const { return has_dll_modules_; }

 private:
  CodegenContext() = default;

  BinaryView binary_;
  RecompilerConfig config_;
  AnalysisState analysis_state_;
  Limits limits_;
  std::unique_ptr<DecodedBinary> decoded_; /**< Created by InitDecoded() */
  std::filesystem::path config_dir_;
  bool is_dll_module_ = false;
  bool has_dll_modules_ = false;
};

}  // namespace rex::codegen
