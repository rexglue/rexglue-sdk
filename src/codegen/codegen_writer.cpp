/**
 * @file        codegen/codegen_writer.cpp
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/codegen_writer.h>
#include "codegen_flags.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/codegen/function_graph.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include "codegen_logging.h"

#include <xxhash.h>

namespace rex::codegen {

constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

CodegenWriter::CodegenWriter(CodegenContext& ctx, Runtime* runtime)
    : ctx_(ctx), runtime_(runtime) {}

// Convenience accessors
FunctionGraph& CodegenWriter::graph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::graph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::binary() const {
  return ctx_.binary();
}
RecompilerConfig& CodegenWriter::config() {
  return ctx_.Config();
}
const RecompilerConfig& CodegenWriter::config() const {
  return ctx_.Config();
}
AnalysisState& CodegenWriter::analysisState() {
  return ctx_.analysisState();
}
const AnalysisState& CodegenWriter::analysisState() const {
  return ctx_.analysisState();
}

bool CodegenWriter::write(bool force) {
  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REXCODEGEN_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                     ctx_.errors.Count());
    return false;
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXCODEGEN_INFO("Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Clean old generated files (from recompile.cpp) ---
  std::string prefix = config().projectName + "_";
  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    auto ext = entry.path().extension();
    if (ext == ".cpp" || ext == ".h" || ext == ".cmake") {
      std::string filename = entry.path().filename().string();
      if (filename == "sources.cmake" || filename.starts_with(prefix) ||
          filename.starts_with("ppc_recomp") || filename.starts_with("ppc_func_mapping") ||
          filename.starts_with("function_table_init") || filename.starts_with("ppc_config")) {
        std::filesystem::remove(entry.path());
      }
    }
  }

  // --- Everything below from recompiler.cpp recompile() ---
  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  const std::string& projectName = config().projectName;

  // Generate {project}_config.h
  REXCODEGEN_TRACE("Recompile: generating {}_config.h", projectName);
  {
    REXCODEGEN_TRACE("  {}_config.h: step 1", projectName);
    println("#pragma once");

    println("#ifndef PPC_CONFIG_H_INCLUDED");
    println("#define PPC_CONFIG_H_INCLUDED\n");
    REXCODEGEN_TRACE("  {}_config.h: step 2", projectName);

    if (config().skipLr)
      println("#define PPC_CONFIG_SKIP_LR");
    if (config().ctrAsLocalVariable)
      println("#define PPC_CONFIG_CTR_AS_LOCAL");
    if (config().xerAsLocalVariable)
      println("#define PPC_CONFIG_XER_AS_LOCAL");
    if (config().reservedRegisterAsLocalVariable)
      println("#define PPC_CONFIG_RESERVED_AS_LOCAL");
    if (config().skipMsr)
      println("#define PPC_CONFIG_SKIP_MSR");
    if (config().crRegistersAsLocalVariables)
      println("#define PPC_CONFIG_CR_AS_LOCAL");
    if (config().nonArgumentRegistersAsLocalVariables)
      println("#define PPC_CONFIG_NON_ARGUMENT_AS_LOCAL");
    if (config().nonVolatileRegistersAsLocalVariables)
      println("#define PPC_CONFIG_NON_VOLATILE_AS_LOCAL");

    println("");

    REXCODEGEN_TRACE(
        "  ppc_config.h: step 3 - binary().baseAddress()=0x{:X}, binary().imageSize()={}",
        binary().baseAddress(), binary().imageSize());
    println("#define PPC_IMAGE_BASE 0x{:X}ull", binary().baseAddress());
    println("#define PPC_IMAGE_SIZE 0x{:X}ull", binary().imageSize());

    REXCODEGEN_TRACE("  ppc_config.h: step 4 - iterating sections");
    size_t codeMin = ~0;
    size_t codeMax = 0;

    for (const auto& section : binary().sections()) {
      if (section.executable) {
        if (section.baseAddress < codeMin)
          codeMin = section.baseAddress;
        if ((section.baseAddress + section.size) > codeMax)
          codeMax = (section.baseAddress + section.size);
      }
    }

    println("#define PPC_CODE_BASE 0x{:X}ull", codeMin);
    println("#define PPC_CODE_SIZE 0x{:X}ull", codeMax - codeMin);

    bool hasRexcrtHeap = config().rexcrtFunctions.contains("RtlAllocateHeap");
    println("");
    println("#define REXCRT_HEAP {}", hasRexcrtHeap ? 1 : 0);

    println("");

    println("#include <rex/ppc/image_info.h>");
    println("extern const rex::PPCImageInfo PPCImageConfig;");

    println("\n#endif");

    REXCODEGEN_TRACE("  {}_config.h: step 5 - saving", projectName);
    SaveCurrentOutData(fmt::format("{}_config.h", projectName));
    REXCODEGEN_TRACE("  {}_config.h: done", projectName);
  }

  // Generate {project}_init.h
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  {
    println("#pragma once\n");
    println("#include \"{}_config.h\"", projectName);
    println("#include <rex/ppc.h>");
    println("#include <rex/logging.h>  // For REX_FATAL on unresolved calls");

    for (const auto* fn : functions) {
      auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
      if (crtIt != rexcrtByAddr.end()) {
        println("PPC_EXTERN_FUNC({});", crtIt->second);
        continue;
      }

      std::string func_name;
      if (fn->base() == analysisState().entryPoint) {
        func_name = "xstart";
      } else if (!fn->name().empty()) {
        func_name = fn->name();
      } else {
        func_name = fmt::format("sub_{:08X}", fn->base());
      }

      println("PPC_EXTERN_IMPORT({});", func_name);
    }

    println("\n// Import function declarations");
    for (const auto& [addr, node] : graph().functions()) {
      if (node->authority() != FunctionAuthority::IMPORT)
        continue;
      println("PPC_EXTERN_IMPORT({});", node->name());
    }

    println("\n// Function mapping table - iterate to register functions with processor");

    SaveCurrentOutData(fmt::format("{}_init.h", projectName));
  }

  // Generate {project}_init.cpp
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp (function mapping table)", projectName);
  {
    println("//=============================================================================");
    println("// ReXGlue Generated - {} Function Mapping Table", projectName);
    println("//=============================================================================\n");
    println("#include \"{}_init.h\"\n", projectName);

    size_t funcMappingCodeMin = ~0ull;
    for (const auto& section : binary().sections()) {
      if (section.executable) {
        if (section.baseAddress < funcMappingCodeMin)
          funcMappingCodeMin = section.baseAddress;
      }
    }

    println("PPCFuncMapping PPCFuncMappings[] = {{");

    for (const auto* fn : functions) {
      if (fn->base() < funcMappingCodeMin)
        continue;

      auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
      if (crtIt != rexcrtByAddr.end()) {
        println("\t{{ 0x{:X}, {} }},", fn->base(), crtIt->second);
        continue;
      }

      std::string func_name;
      if (fn->base() == analysisState().entryPoint) {
        func_name = "xstart";
      } else if (!fn->name().empty()) {
        func_name = fn->name();
      } else {
        func_name = fmt::format("sub_{:08X}", fn->base());
      }

      println("\t{{ 0x{:X}, {} }},", fn->base(), func_name);
    }

    for (const auto& [addr, node] : graph().functions()) {
      if (node->authority() != FunctionAuthority::IMPORT)
        continue;
      println("\t{{ 0x{:X}, {} }},", addr, node->name());
    }

    println("\t{{ 0, nullptr }}");
    println("}};");

    SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));
  }

  // Generate {project}_config.cpp
  REXCODEGEN_TRACE("Recompile: generating {}_config.cpp (PPCImageConfig)", projectName);
  {
    println("//=============================================================================");
    println("// ReXGlue Generated - {} Image Configuration", projectName);
    println("//=============================================================================\n");
    println("#include \"{}_init.h\"\n", projectName);
    println("#include <rex/ppc/image_info.h>");
    println("");
    println("const rex::PPCImageInfo PPCImageConfig = {{");
    println("    PPC_CODE_BASE,      // code_base");
    println("    PPC_CODE_SIZE,      // code_size");
    println("    PPC_IMAGE_BASE,     // image_base");
    println("    PPC_IMAGE_SIZE,     // image_size");
    println("    PPCFuncMappings,    // func_mappings");
    println("    REXCRT_HEAP,        // rexcrt_heap");
    println("}};");

    SaveCurrentOutData(fmt::format("{}_config.cpp", projectName));
  }

  // Filter out imports and rexcrt functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });

  // Build EmitContext -- resolver is now properly connected
  EmitContext emitCtx{binary(), config(), graph(),
                      static_cast<uint32_t>(analysisState().entryPoint), nullptr};
  if (runtime_)
    emitCtx.resolver = runtime_->export_resolver();

  // Generate recomp files with size-based splitting
  REXCODEGEN_INFO("Recompiling {} functions...", functions.size());
  size_t currentFileBytes = 0;
  println("#include \"{}_init.h\"\n", projectName);

  for (size_t i = 0; i < functions.size(); i++) {
    std::string code = functions[i]->emitCpp(emitCtx);

    if (currentFileBytes > 0 && currentFileBytes + code.size() > REXCVAR_GET(max_file_size_bytes)) {
      SaveCurrentOutData();
      println("#include \"{}_init.h\"\n", projectName);
      currentFileBytes = 0;
    }

    if (code.size() > REXCVAR_GET(max_file_size_bytes)) {
      REXCODEGEN_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})",
                      functions[i]->base(), code.size(), REXCVAR_GET(max_file_size_bytes));
    }

    out += code;
    currentFileBytes += code.size();
  }

  SaveCurrentOutData();
  REXCODEGEN_INFO("Recompilation complete.");

  // Generate sources.cmake
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    println("# Auto-generated by rexglue codegen - DO NOT EDIT");
    println("#");
    println("# IMPORTANT: For SEH (Structured Exception Handling) support on Windows,");
    println("# add /EHa to your compile options:");
    println("#   target_compile_options(your_target PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHa>)");
    println("#");
    println("set(GENERATED_SOURCES");
    println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_config.cpp", projectName);
    println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_init.cpp", projectName);
    for (size_t i = 0; i < cppFileIndex; ++i) {
      println("    ${{CMAKE_CURRENT_LIST_DIR}}/{}_recomp.{}.cpp", projectName, i);
    }
    println(")");

    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  FlushPendingWrites();
  return true;
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    std::string filename;

    if (name.empty()) {
      filename = fmt::format("{}_recomp.{}.cpp", config().projectName, cppFileIndex);
      ++cppFileIndex;
    } else {
      filename = std::string(name);
    }

    pendingWrites.emplace_back(std::move(filename), std::move(out));
    out.clear();
  }
}

void CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  for (const auto& [filename, content] : pendingWrites) {
    std::string filePath = (outputPath / filename).string();
    REXCODEGEN_TRACE("flush_pending_writes: filePath={}", filePath);

    bool shouldWrite = true;

    FILE* f = fopen(filePath.c_str(), "rb");
    if (f) {
      std::vector<uint8_t> temp;

      fseek(f, 0, SEEK_END);
      long fileSize = ftell(f);
      if (fileSize == static_cast<long>(content.size())) {
        fseek(f, 0, SEEK_SET);
        temp.resize(fileSize);
        fread(temp.data(), 1, fileSize, f);

        shouldWrite = !XXH128_isEqual(XXH3_128bits(temp.data(), temp.size()),
                                      XXH3_128bits(content.data(), content.size()));
      }
      fclose(f);
    }

    if (shouldWrite) {
      f = fopen(filePath.c_str(), "wb");
      if (!f) {
        REXCODEGEN_ERROR("Failed to open file for writing: {}", filePath);
        continue;
      }
      fwrite(content.data(), 1, content.size(), f);
      fclose(f);
      REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath);
    }
  }

  pendingWrites.clear();
}

}  // namespace rex::codegen
