/**
 * @file        rexcodegen/output.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// TODO(tomc): this file is sorta legacy from older iterations of the codegen.
// It should be refactored to be more of a pure output module,
// that just takes in data and emits files, without any logic
// related to how the data is generated.
//
// The function graph should be used only for resolving imports, and all other data should be passed
// in directly.

#include <algorithm>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/output.h>
#include <rex/logging.h>

#include "codegen_flags.h"
#include "codegen_logging.h"

// Minimum SDK version required by code this codegen version emits.
// Bump this when codegen starts emitting code that uses new SDK APIs.
static constexpr const char* kMinSdkVersion = "0.3.0";

namespace rex::codegen {

//=============================================================================
// Configuration
//=============================================================================

void RecompilerOutput::set_config(const OutputConfig& config) {
  config_ = config;
}

//=============================================================================
// Function Management
//=============================================================================

void RecompilerOutput::add_function(guest_addr_t address, const std::string& name,
                                    std::string cpp_code) {
  CompiledFunction func;
  func.address = address;
  func.name = name;
  func.cpp_code = std::move(cpp_code);
  functions_[address] = std::move(func);
}

void RecompilerOutput::add_jump_table(guest_addr_t func_address, const JumpTable& jump_table) {
  auto it = functions_.find(func_address);
  if (it != functions_.end()) {
    it->second.jump_tables.push_back(jump_table);
  }
}

//=============================================================================
// Output Generation
//=============================================================================

bool RecompilerOutput::write_all(const std::filesystem::path& output_dir) {
  namespace fs = std::filesystem;

  generated_files_.clear();

  // Create output directory
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec) {
    REXCODEGEN_ERROR("Failed to create output directory: {}", ec.message());
    return false;
  }

  REXCODEGEN_INFO("Writing recompiled output to: {}", output_dir.string());
  REXCODEGEN_INFO("  Functions: {}", functions_.size());

  // Write all output files
  if (!write_config_h(output_dir))
    return false;
  if (!write_config_cpp(output_dir))
    return false;
  if (!write_shared_h(output_dir))
    return false;
  if (!write_recomp_files(output_dir))
    return false;
  if (!write_init(output_dir))
    return false;
  if (config_.emit_cmake && !write_cmake(output_dir))
    return false;

  REXCODEGEN_INFO("Generated {} files", generated_files_.size());
  return true;
}

//=============================================================================
// {project_name}_config.h
//=============================================================================

bool RecompilerOutput::write_config_h(const std::filesystem::path& dir) {
  auto filename = fmt::format("{}_config.h", config_.project_name);
  auto path = dir / filename;
  std::ofstream out(path);
  if (!out) {
    REXCODEGEN_ERROR("Failed to create: {}", path.string());
    return false;
  }

  out << "#pragma once\n";
  out << "//=============================================================================\n";
  out << fmt::format("// ReXGlue Generated - {} Configuration\n", config_.project_name);
  out << "//=============================================================================\n\n";

  out << fmt::format("#define PPC_IMAGE_BASE 0x{:08X}ull\n", config_.image_base);
  out << fmt::format("#define PPC_IMAGE_SIZE 0x{:X}ull\n", config_.image_size);
  out << fmt::format("#define PPC_CODE_BASE 0x{:08X}ull\n", config_.code_base);
  out << fmt::format("#define PPC_CODE_SIZE 0x{:X}ull\n", config_.code_size);
  out << "\n";
  out << fmt::format("#define REXCRT_HEAP {}\n", config_.rexcrt_heap ? 1 : 0);
  out << "\n";

  // Prebuilt PPCImageInfo (defined in {project}_config.cpp)
  out << "#include <rex/ppc/image_info.h>\n";
  out << "extern const rex::PPCImageInfo PPCImageConfig;\n";
  out << "\n";

  generated_files_.push_back(filename);
  return true;
}

//=============================================================================
// {project_name}_config.cpp
//=============================================================================

bool RecompilerOutput::write_config_cpp(const std::filesystem::path& dir) {
  auto filename = fmt::format("{}_config.cpp", config_.project_name);
  auto path = dir / filename;
  std::ofstream out(path);
  if (!out) {
    REXCODEGEN_ERROR("Failed to create: {}", path.string());
    return false;
  }

  out << "//=============================================================================\n";
  out << fmt::format("// ReXGlue Generated - {} Image Configuration\n", config_.project_name);
  out << "//=============================================================================\n\n";
  out << fmt::format("#include \"{}_init.h\"\n\n", config_.project_name);
  out << "#include <rex/ppc/image_info.h>\n\n";
  out << "const rex::PPCImageInfo PPCImageConfig = {\n";
  out << "    PPC_CODE_BASE,      // code_base\n";
  out << "    PPC_CODE_SIZE,      // code_size\n";
  out << "    PPC_IMAGE_BASE,     // image_base\n";
  out << "    PPC_IMAGE_SIZE,     // image_size\n";
  out << "    PPCFuncMappings,    // func_mappings\n";
  out << "    REXCRT_HEAP,        // rexcrt_heap\n";
  out << "};\n";

  generated_files_.push_back(filename);
  return true;
}

//=============================================================================
// {project_name}_init.h - Forward Declarations
//=============================================================================

bool RecompilerOutput::write_shared_h(const std::filesystem::path& dir) {
  auto filename = fmt::format("{}_init.h", config_.project_name);
  auto path = dir / filename;
  std::ofstream out(path);
  if (!out) {
    REXCODEGEN_ERROR("Failed to create: {}", path.string());
    return false;
  }

  out << "#pragma once\n";
  out << "//=============================================================================\n";
  out << fmt::format("// ReXGlue Generated - {} Function Declarations\n", config_.project_name);
  out << "//=============================================================================\n\n";
  out << fmt::format("#include \"{}_config.h\"\n", config_.project_name);
  out << "#include <atomic>\n";
  out << "#include <rex/ppc.h>\n\n";

  // Forward declare import functions first (from graph)
  if (graph_) {
    bool hasImports = false;
    for (const auto& [addr, fn] : graph_->functions()) {
      if (fn->authority() == FunctionAuthority::IMPORT) {
        if (!hasImports) {
          out << "// Import function declarations\n";
          hasImports = true;
        }
        out << fmt::format("PPC_EXTERN_FUNC({});\n", fn->name());
      }
    }
    if (hasImports)
      out << "\n";
  }

  // Forward declare all recompiled functions
  out << "// Recompiled function declarations\n";
  for (const auto& [addr, func] : functions_) {
    out << fmt::format("PPC_EXTERN_FUNC({});\n", func.name);
  }

  generated_files_.push_back(filename);
  return true;
}

//=============================================================================
// {project_name}_recomp.N.cpp - Function Implementation Files
//=============================================================================

bool RecompilerOutput::write_recomp_files(const std::filesystem::path& dir) {
  if (functions_.empty())
    return true;

  // Collect functions into vector for indexed access
  std::vector<const CompiledFunction*> func_list;
  func_list.reserve(functions_.size());
  for (const auto& [addr, func] : functions_) {
    func_list.push_back(&func);
  }

  // Calculate number of files needed
  size_t num_files =
      (func_list.size() + REXCVAR_GET(functions_per_file) - 1) / REXCVAR_GET(functions_per_file);

  for (size_t file_idx = 0; file_idx < num_files; ++file_idx) {
    auto filename = fmt::format("{}_recomp.{}.cpp", config_.project_name, file_idx);
    auto path = dir / filename;
    std::ofstream out(path);
    if (!out) {
      REXCODEGEN_ERROR("Failed to create: {}", path.string());
      return false;
    }

    out << "//=============================================================================\n";
    out << fmt::format("// ReXGlue Generated - {} Recompiled Functions (Part {})\n",
                       config_.project_name, file_idx);
    out << "//=============================================================================\n\n";
    out << fmt::format("#include \"{}_init.h\"\n\n", config_.project_name);

    // Write functions for this file
    size_t start_idx = file_idx * REXCVAR_GET(functions_per_file);
    size_t end_idx = std::min(start_idx + REXCVAR_GET(functions_per_file), func_list.size());

    for (size_t i = start_idx; i < end_idx; ++i) {
      const auto& func = *func_list[i];
      out << get_function_definition(func);
      out << "\n";
    }

    generated_files_.push_back(filename);
  }

  REXCODEGEN_INFO("  Generated {} function files", num_files);
  return true;
}

//=============================================================================
// {project_name}_init.cpp - Combined Function Mapping & Initialization
//=============================================================================

bool RecompilerOutput::write_init(const std::filesystem::path& dir) {
  auto filename = fmt::format("{}_init.cpp", config_.project_name);
  auto path = dir / filename;
  std::ofstream out(path);
  if (!out) {
    REXCODEGEN_ERROR("Failed to create: {}", path.string());
    return false;
  }

  out << "//=============================================================================\n";
  out << fmt::format("// ReXGlue Generated - {} Function Mapping & Initialization\n",
                     config_.project_name);
  out << "// Contains function mapping table and runtime registration\n";
  out << "//=============================================================================\n\n";
  out << fmt::format("#include \"{}_init.h\"\n", config_.project_name);
  out << "#include <rex/runtime.h>\n";
  out << "#include <rex/system/processor.h>\n";
  out << "#include <cstdio>\n";
  out << "#include <cstdlib>\n\n";

  // Function mapping table
  out << "//=============================================================================\n";
  out << "// Function Mapping Table\n";
  out << "//=============================================================================\n\n";

  out << "PPCFuncMapping PPCFuncMappings[] = {\n";

  // Include BOTH recompiled functions AND imports in the mapping table
  std::vector<std::pair<guest_addr_t, std::string>> all_entries;
  all_entries.reserve(functions_.size());

  // Add recompiled functions
  for (const auto& [addr, func] : functions_) {
    all_entries.emplace_back(addr, func.name);
  }

  // Add imports from graph (FunctionNodes with IMPORT authority)
  if (graph_) {
    for (const auto& [addr, fn] : graph_->functions()) {
      if (fn->authority() == FunctionAuthority::IMPORT && addr != 0) {
        all_entries.emplace_back(addr, std::string(fn->name()));
      }
    }
  }

  // Sort by address
  std::sort(all_entries.begin(), all_entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  for (const auto& [addr, name] : all_entries) {
    out << fmt::format("    {{ 0x{:08X}, {} }},\n", addr, name);
  }

  out << "    { 0, nullptr }  // Null terminator\n";
  out << "};\n\n";

  // Function table initialization
  out << "//=============================================================================\n";
  out << "// Function Table Initialization\n";
  out << "//=============================================================================\n\n";

  out << "void init_function_table() {\n";
  out << "    auto* runtime = rex::Runtime::instance();\n";
  out << "    if (!runtime) {\n";
  out << "        fprintf(stderr, \"FATAL: Runtime not initialized in init_function_table\\n\");\n";
  out << "        abort();\n";
  out << "    }\n";
  out << "    auto* processor = runtime->processor();\n\n";

  out << "    // Register ALL functions from unified PPCFuncMappings array\n";
  out << "    // This includes both recompiled functions and import thunks\n";
  out << "    for (int i = 0; PPCFuncMappings[i].guest != 0; ++i) {\n";
  out << "        if (PPCFuncMappings[i].host != nullptr) {\n";
  out << "            processor->SetFunction(\n";
  out << "                static_cast<uint32_t>(PPCFuncMappings[i].guest),\n";
  out << "                PPCFuncMappings[i].host);\n";
  out << "        }\n";
  out << "    }\n";
  out << "}\n";

  generated_files_.push_back(filename);
  return true;
}

//=============================================================================
// CMakeLists.txt
//=============================================================================

bool RecompilerOutput::write_cmake(const std::filesystem::path& dir) {
  auto path = dir / "CMakeLists.txt";

  // Collect all cpp files from generated files
  std::vector<std::string> cpp_files;
  for (const auto& file : generated_files_) {
    if (file.ends_with(".cpp")) {
      cpp_files.push_back(file);
    }
  }

  auto lib_name = fmt::format("{}_recompiled", config_.project_name);
  auto shared_header = fmt::format("{}_init.h", config_.project_name);

  // Generate content to string first (for comparison)
  std::ostringstream out;

  out << fmt::format("# Generated by ReXGlue - {} Recompiled Code Library\n", config_.project_name);
  out << "# \n";
  out << fmt::format("# Usage: Include this directory in your project and link against {}.\n",
                     lib_name);
  out << "# You must also provide the rexglue runtime headers.\n";
  out << "#\n";
  out << "# Example:\n";
  out << "#   set(REXGLUE_DIR \"/path/to/rexglue\")\n";
  out << fmt::format("#   add_subdirectory(generated {})\n", lib_name);
  out << "#   target_include_directories(your_target PRIVATE ${REXGLUE_DIR}/include)\n";
  out << fmt::format("#   target_link_libraries(your_target {})\n", lib_name);
  out << "#\n\n";

  out << "cmake_minimum_required(VERSION 3.20)\n";
  out << fmt::format("project({})\n\n", lib_name);
  out << "set(CMAKE_CXX_STANDARD 23)\n";
  out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

  out << fmt::format("# {} recompiled code library\n", config_.project_name);
  out << "# Includes recompiled functions and function table initialization\n";
  out << fmt::format("add_library({} STATIC\n", lib_name);
  for (const auto& file : cpp_files) {
    out << fmt::format("    {}\n", file);
  }
  out << ")\n\n";

  out << fmt::format("target_include_directories({} PUBLIC ${{CMAKE_CURRENT_SOURCE_DIR}})\n\n",
                     lib_name);

  out << "# Allow user to specify rexglue include path\n";
  out << "if(REXGLUE_INCLUDE_DIR)\n";
  out << fmt::format("    target_include_directories({} PRIVATE ${{REXGLUE_INCLUDE_DIR}})\n",
                     lib_name);
  out << "endif()\n\n";

  out << "# Compile options\n";
  out << "if(WIN32)\n";
  out << fmt::format("    target_compile_options({} PRIVATE /fp:strict /EHa)\n", lib_name);
  out << "    # /EHa enables asynchronous exception handling for SEH support\n";
  out << "else()\n";
  out << fmt::format("    target_compile_options({} PRIVATE\n", lib_name);
  out << "        -fno-strict-aliasing\n";
  out << "        -ffp-model=strict\n";
  out << "        -msse4.1\n";
  out << "    )\n";
  out << "endif()\n\n";

  out << "# Precompiled headers for faster builds\n";
  out << fmt::format("target_precompile_headers({} PUBLIC {})\n", lib_name, shared_header);

  std::string new_content = out.str();

  // Check if file exists and has same content (avoid CMake reconfiguration)
  if (std::filesystem::exists(path)) {
    std::ifstream existing(path);
    if (existing) {
      std::string existing_content((std::istreambuf_iterator<char>(existing)),
                                   std::istreambuf_iterator<char>());
      if (new_content == existing_content) {
        // Content unchanged, skip write to avoid triggering CMake reconfiguration
        REXCODEGEN_DEBUG("CMakeLists.txt unchanged, skipping write");
        generated_files_.push_back("CMakeLists.txt");
        return true;
      }
    }
  }

  // Write new content
  std::ofstream file(path);
  if (!file) {
    REXCODEGEN_ERROR("Failed to create: {}", path.string());
    return false;
  }
  file << new_content;

  generated_files_.push_back("CMakeLists.txt");
  return true;
}

//=============================================================================
// Helpers
//=============================================================================

std::string RecompilerOutput::get_function_declaration(const CompiledFunction& func) const {
  return fmt::format("PPC_EXTERN_FUNC({});\n", func.name);
}

std::string RecompilerOutput::get_function_definition(const CompiledFunction& func) const {
  std::ostringstream out;

  if (config_.emit_comments) {
    out << fmt::format("// Function at 0x{:08X}\n", func.address);
  }

  // CPPEmitter generates complete function with signature, prologue, body, and closing brace
  out << func.cpp_code;

  return out.str();
}

}  // namespace rex::codegen
