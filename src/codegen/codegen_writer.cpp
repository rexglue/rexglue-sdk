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
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include "codegen_logging.h"
#include "template_registry_internal.h"

#include <xxhash.h>

namespace {

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& rexcrtByAddr) {
  const auto& cfg = ctx.Config();

  // Compute code_base and code_size from binary sections
  size_t codeMin = ~size_t(0);
  size_t codeMax = 0;
  for (const auto& section : ctx.binary().sections()) {
    if (section.executable) {
      if (section.baseAddress < codeMin)
        codeMin = section.baseAddress;
      if ((section.baseAddress + section.size) > codeMax)
        codeMax = section.baseAddress + section.size;
    }
  }

  // Build functions JSON array
  nlohmann::json functionsJson = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string funcName;
    bool isRexcrt = false;

    auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
    if (crtIt != rexcrtByAddr.end()) {
      funcName = crtIt->second;
      isRexcrt = true;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = fmt::format("sub_{:08X}", fn->base());
    }

    functionsJson.push_back({
        {"address", fmt::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"is_rexcrt", isRexcrt},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", false},
    });
  }

  // Build imports JSON array
  nlohmann::json importsJson = nlohmann::json::array();
  for (const auto& [addr, node] : ctx.graph.functions()) {
    if (node->authority() != rex::codegen::FunctionAuthority::IMPORT)
      continue;
    importsJson.push_back({
        {"address", fmt::format("0x{:X}", addr)},
        {"name", node->name()},
    });
  }

  // Build config flags
  nlohmann::json configFlags = {
      {"skip_lr", cfg.skipLr},
      {"ctr_as_local", cfg.ctrAsLocalVariable},
      {"xer_as_local", cfg.xerAsLocalVariable},
      {"reserved_as_local", cfg.reservedRegisterAsLocalVariable},
      {"skip_msr", cfg.skipMsr},
      {"cr_as_local", cfg.crRegistersAsLocalVariables},
      {"non_argument_as_local", cfg.nonArgumentRegistersAsLocalVariables},
      {"non_volatile_as_local", cfg.nonVolatileRegistersAsLocalVariables},
  };

  return {
      {"project", cfg.projectName},
      {"image_base", fmt::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", fmt::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", fmt::format("0x{:X}", codeMin)},
      {"code_size", fmt::format("0x{:X}", codeMax - codeMin)},
      {"rexcrt_heap", cfg.rexcrtFunctions.contains("RtlAllocateHeap") ? 1 : 0},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"imports", importsJson},
      {"recomp_files", nlohmann::json::array()},
  };
}

}  // namespace

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

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, rexcrtByAddr);

  // Generate {project}_init.h (self-contained: config + declarations + macros)
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.h", projectName));

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));

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
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    for (size_t i = 0; i < cppFileIndex; ++i) {
      recompFiles.push_back(fmt::format("{}_recomp.{}.cpp", projectName, i));
    }
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
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
