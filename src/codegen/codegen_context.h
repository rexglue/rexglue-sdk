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

#include "analysis_error_log.h"
#include "binary_view.h"
#include "function_scanner.h"
#include "config.h"
#include "function_graph.h"
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::codegen {

/**
 * Analysis limits. Passed by the tool, hashed into the input fingerprint.
 */
struct Limits {
  /** Largest generated source file in bytes before the writer starts a new one. */
  u32 max_file_size_bytes = 1048576;
  /** Discovery passes before giving up on a fixed point. */
  u32 max_discovery_iterations = 1000;
  u32 max_vtable_iterations = 100;
  /** Merge resolution passes. */
  u32 max_resolve_iterations = 100;
  /** C++ exception states a function may declare. */
  u32 max_eh_states = 100;
  /** C++ try blocks a function may declare. */
  u32 max_eh_try_blocks = 50;
  /** C++ IP to state map entries a function may declare. */
  u32 max_eh_ip_map_entries = 200;
  /** SEH scope entries a function may declare. */
  u32 max_seh_scope_entries = 100;
  /** Instructions scanned backward for a jump table base. */
  u32 backward_scan_limit = 64;
  u32 max_jump_table_entries = 512;
};

class DecodedBinary;
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

  /**
   * Entry addresses of the chunks of each parent function, keyed by parent address, from the
   * `parent` field of the function configs.
   */
  std::unordered_map<u32, std::vector<u32>> chunks_by_parent;

  /** Save and restore millicode runs found by analysis. */
  std::vector<MillicodeRun> millicode_runs;

  /** A function another user module exports by ordinal, imported through a thunk. */
  struct ModuleImportRef {
    std::string library; /**< Base name of the exporting module. */
    u16 ordinal = 0;
  };
  /** Imports of functions that a user module exports, keyed by the import thunk address. */
  std::unordered_map<u32, ModuleImportRef> module_imports;

  std::unordered_map<u32, u32> invalid_instructions; /**< Maps address to size */
  /**
   * Entry addresses of exception handler functions, copied from `analysis.exception_handler_funcs`
   * in the TOML.
   */
  std::vector<u32> exception_handler_funcs;
  /** Entry addresses found in SEH scope tables, queued as functions after the .pdata functions. */
  std::vector<u32> eh_discovered_funcs;
};

/** Owns the binary, function graph, config, analysis errors and scan artifacts of the pipeline. */
class CodegenContext : public rex::MoveOnly {
 public:
  /**
   * Loads the TOML config and the XEX through the runtime and builds the BinaryView from the
   * module.
   *
   * @param runtime Runtime whose content root holds the XEX
   * @return The context, or an error
   */
  [[nodiscard]] static Result<CodegenContext> Create(const std::filesystem::path& config_path,
                                                     rex::kernel::TitleRuntime& runtime);

  /**
   * @param binary The BinaryView, moved into the context
   * @param config The RecompilerConfig, moved into the context
   */
  static CodegenContext Create(BinaryView binary, RecompilerConfig config);

  CodegenContext(CodegenContext&&) noexcept(false);
  CodegenContext& operator=(CodegenContext&&) noexcept(false);
  ~CodegenContext();

  FunctionGraph graph; /**< All functions including imports */
  /** Errors the analysis phases collected, which stop the writer unless forced. */
  AnalysisErrorLog errors;

  /** Scan phase artifacts passed to Discover for scanner setup. */
  struct ScanArtifacts {
    ScanArtifacts() = default;
    ScanArtifacts(const ScanArtifacts&) = default;
    ScanArtifacts& operator=(const ScanArtifacts&) = default;
    ScanArtifacts(ScanArtifacts&&) noexcept(false) = default;
    ScanArtifacts& operator=(ScanArtifacts&&) noexcept(false) = default;
    ~ScanArtifacts() = default;

    std::vector<CodeRegion> code_regions; /**< Null delimited code regions */
    /**
     * Address ranges `[first, second)` of runs of at least `data_region_threshold` words equal to
     * 0 or 0xFFFFFFFF.
     */
    std::vector<std::pair<u32, u32>> data_regions;
    std::unordered_map<u32, u32> pdata_sizes; /**< Maps address to size */
  } scan;

  const BinaryView& GetBinary() const { return binary_; }
  BinaryView& GetBinary() { return binary_; }

  /** Returns the decoded binary. Valid only after InitDecoded(). */
  DecodedBinary& GetDecoded();
  /** Returns the decoded binary. Valid only after InitDecoded(). */
  const DecodedBinary& GetDecoded() const;

  /**
   * Creates the DecodedBinary once the context is at its final location. Runs once after
   * Create() and before GetDecoded().
   */
  void InitDecoded();

  /** Reports whether InitDecoded() has run. */
  bool HasDecoded() const { return decoded_ != nullptr; }

  RecompilerConfig& GetConfig() { return config_; }
  const RecompilerConfig& GetConfig() const { return config_; }

  AnalysisState& GetAnalysisState() { return analysis_state_; }
  const AnalysisState& GetAnalysisState() const { return analysis_state_; }

  /** Returns the analysis limits for every phase run against this context. */
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
  std::unique_ptr<DecodedBinary> decoded_;
  std::filesystem::path config_dir_;
  bool is_dll_module_ = false;
  bool has_dll_modules_ = false;
};

}  // namespace rex::codegen
