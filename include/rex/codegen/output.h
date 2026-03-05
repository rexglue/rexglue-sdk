/**
 * @file        rexcodegen/internal/output.h
 * @brief       Output writer interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <rex/codegen/recompiled_function.h>
#include <rex/types.h>

namespace rex::codegen {

//=============================================================================
// Output Configuration
//=============================================================================

/**
 * Configuration for recompiler output generation
 */
struct OutputConfig {
  // Image metadata
  uint64_t image_base = 0;
  uint64_t image_size = 0;
  uint64_t code_base = 0;
  uint64_t code_size = 0;

  // Output options
  std::string project_name = "rex";
  bool emit_comments = true;
  bool emit_cmake = true;
  bool rexcrt_heap = false;
};

//=============================================================================
// Compiled Function Entry
//=============================================================================

/**
 * A single recompiled function
 */
struct CompiledFunction {
  guest_addr_t address;
  std::string name;
  std::string cpp_code;
  std::vector<JumpTable> jump_tables;
};

//=============================================================================
// Recompiler Output Class
//=============================================================================

/**
 * Recompiler Output Generator
 *
 * Collects recompiled functions and generates a standalone C++ project:
 * - ppc_config.h - Image constants
 * - ppc_context.h - PPCContext structure (copies from ppc_runtime.h)
 * - ppc_init.h - Forward declarations for all functions
 * - ppc_recomp.N.cpp - Function implementation files
 * - ppc_func_mapping.cpp - Function lookup table
 * - CMakeLists.txt - Build configuration
 *
 * Usage:
 *   RecompilerOutput output;
 *   output.set_config({.image_base = 0x82000000, ...});
 *   for (auto& func : functions) {
 *       output.add_function(func.address, func.name, cpp_code);
 *   }
 *   output.write_all(output_dir);
 */
class RecompilerOutput {
 public:
  RecompilerOutput() = default;

  /**
   * Set output configuration
   */
  void set_config(const OutputConfig& config);

  /**
   * Get current configuration
   */
  const OutputConfig& config() const { return config_; }

  /**
   * Add a recompiled function
   * @param address Function guest address
   * @param name Function name (e.g., "sub_82060000")
   * @param cpp_code Generated C++ code for the function body
   */
  void add_function(guest_addr_t address, const std::string& name, std::string cpp_code);

  /**
   * Add a jump table for a function
   * @param func_address Function containing the jump table
   * @param jump_table Jump table metadata
   */
  void add_jump_table(guest_addr_t func_address, const JumpTable& jump_table);

  /**
   * Set graph for import resolution
   * Imports are read from FunctionNodes with IMPORT authority
   * @param graph FunctionGraph containing imports (can be null)
   */
  void set_graph(const FunctionGraph* graph) { graph_ = graph; }

  /**
   * Get number of functions added
   */
  size_t function_count() const { return functions_.size(); }

  /**
   * Write all output files to the specified directory
   * @param output_dir Output directory (created if doesn't exist)
   * @return true on success, false on failure
   */
  bool write_all(const std::filesystem::path& output_dir);

  /**
   * Get list of generated file names (after write_all)
   */
  const std::vector<std::string>& generated_files() const { return generated_files_; }

 private:
  OutputConfig config_;
  std::map<guest_addr_t, CompiledFunction> functions_;  // Sorted by address
  const FunctionGraph* graph_ = nullptr;                // For imports (IMPORT authority nodes)
  std::vector<std::string> generated_files_;

  // File generators
  bool write_config_h(const std::filesystem::path& dir);
  bool write_config_cpp(const std::filesystem::path& dir);
  bool write_shared_h(const std::filesystem::path& dir);
  bool write_recomp_files(const std::filesystem::path& dir);
  bool write_init(const std::filesystem::path& dir);  // Combined func mapping + table init
  bool write_cmake(const std::filesystem::path& dir);

  // Helpers
  std::string get_function_declaration(const CompiledFunction& func) const;
  std::string get_function_definition(const CompiledFunction& func) const;
};

}  // namespace rex::codegen
