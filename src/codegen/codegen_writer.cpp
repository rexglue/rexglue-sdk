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

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <format>
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/output_partition.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "file_io.h"
#include "mmio_analysis.h"
#include "template_registry_internal.h"

namespace {

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& shimByAddr) {
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

    auto shimIt = shimByAddr.find(fn->base());
    if (shimIt != shimByAddr.end()) {
      funcName = shimIt->second;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = std::format("sub_{:08X}", fn->base());
    }

    functionsJson.push_back({
        {"address", std::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", fn->authority() == rex::codegen::FunctionAuthority::IMPORT},
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
      {"image_base", std::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", std::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", std::format("0x{:X}", codeMin)},
      {"code_size", std::format("0x{:X}", codeMax - codeMin)},
      {"shim_heap", cfg.UsesShimHeap() ? 1 : 0},
      {"thunk_reserve_size", std::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.hasDllModules()},
      {"is_dll", ctx.isDllModule()},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"recomp_files", nlohmann::json::array()},
  };
}

}  // namespace

namespace rex::codegen {

namespace {

bool IsGeneratedOutputName(std::string_view filename, std::string_view projectName) {
  auto dot = filename.rfind('.');
  if (dot == std::string_view::npos)
    return false;
  auto ext = filename.substr(dot);
  if (ext != ".cpp" && ext != ".h" && ext != ".cmake")
    return false;

  if (filename == "sources.cmake")
    return true;

  constexpr std::array<std::string_view, 4> kPrefixes{"ppc_recomp", "ppc_func_mapping",
                                                      "function_table_init", "ppc_config"};
  for (auto prefix : kPrefixes) {
    if (filename.starts_with(prefix))
      return true;
  }

  std::string projectPrefix(projectName);
  projectPrefix += '_';
  return filename.starts_with(projectPrefix);
}

}  // namespace

constexpr size_t kOutputBufferReserveSize = size_t{32} * 1024 * 1024;

namespace {

std::string HexAddress(u32 address) {
  return std::format("0x{:08X}", address);
}

std::string_view BlockName(MMIOAccess access) {
  return access == MMIOAccess::GPU ? "GPU" : "APU";
}

nlohmann::json BuildMMIOReport(const std::vector<MMIOReportSite>& sites) {
  nlohmann::json report{{"proven", nlohmann::json::array()},
                        {"declared", nlohmann::json::array()},
                        {"guarded", nlohmann::json::array()},
                        {"unserved", nlohmann::json::array()},
                        {"unsupported", nlohmann::json::array()}};
  for (const MMIOReportSite& entry : sites) {
    nlohmann::json row{{"function", HexAddress(entry.function)},
                       {"address", HexAddress(entry.site.address)},
                       {"access", entry.site.write ? "write" : "read"}};
    switch (entry.site.reason) {
      case MMIOReason::Proven:
        row["block"] = BlockName(entry.site.access);
        row["target"] = HexAddress(entry.site.target);
        report["proven"].push_back(std::move(row));
        break;
      case MMIOReason::Offset:
        row["reason"] = "offset";
        report["guarded"].push_back(std::move(row));
        break;
      case MMIOReason::Barrier:
        row["reason"] = "eieio";
        report["guarded"].push_back(std::move(row));
        break;
      case MMIOReason::Declared:
        row["block"] = BlockName(entry.site.access);
        report["declared"].push_back(std::move(row));
        break;
      case MMIOReason::Unserved:
        row["target"] = HexAddress(entry.site.target);
        report["unserved"].push_back(std::move(row));
        break;
      case MMIOReason::Unsupported:
        row["target"] = HexAddress(entry.site.target);
        report["unsupported"].push_back(std::move(row));
        break;
    }
  }
  return report;
}

}  // namespace

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
  deletedFiles_.clear();
  writtenFiles_.clear();
  unchangedFiles_.clear();

  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REX_FAIL(codegen, false,
             "Code generation blocked: {} validation errors. Use --force to override.",
             ctx_.errors.Count());
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXLOG_TRACE(codegen, "Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Everything below from recompiler.cpp recompile() ---
  REXLOG_TRACE(codegen, "Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build shim reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> shimByAddr;
  for (const auto& [name, addr] : config().shimFunctions) {
    auto shimName = std::format("rexshim_{}", name);
    shimByAddr[addr] = shimName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(shimName));
    }
  }

  const std::string& projectName = config().projectName;

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, shimByAddr);

  // Generate {project}_pch.h (config + macros, stable enough to precompile)
  REXLOG_TRACE(codegen, "Recompile: generating {}_pch.h", projectName);
  out = renderWithJson(registry, "codegen/pch_h", tmplData);
  SaveCurrentOutData(std::format("{}_pch.h", projectName));

  // Generate {project}_funcs.h (every guest function declaration)
  REXLOG_TRACE(codegen, "Recompile: generating {}_funcs.h", projectName);
  out = renderWithJson(registry, "codegen/funcs_h", tmplData);
  SaveCurrentOutData(std::format("{}_funcs.h", projectName));

  // Generate {project}_init.h (the full surface, for init.cpp and consumers)
  REXLOG_TRACE(codegen, "Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(std::format("{}_init.h", projectName));

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXLOG_TRACE(codegen, "Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(std::format("{}_init.cpp", projectName));

  // Generate {project}_register.cpp (registration function for hash-based dispatch)
  REXLOG_TRACE(codegen, "Recompile: generating {}_register.cpp", projectName);
  tmplData["is_dll"] = ctx_.isDllModule();
  out = renderWithJson(registry, "codegen/register_cpp", tmplData);
  SaveCurrentOutData(std::format("{}_register.cpp", projectName));

  // Filter out imports and shim functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions,
                [&shimByAddr](const FunctionNode* fn) { return shimByAddr.contains(fn->base()); });

  EmitContext emitCtx{binary(), config(), graph(), ctx_.limits(),
                      static_cast<uint32_t>(analysisState().entryPoint)};

  REXLOG_TRACE(codegen, "Recompiling {} functions...", functions.size());

  std::vector<std::string> bodies;
  std::vector<FunctionSize> sizes;
  std::vector<std::unordered_set<std::string>> references;
  bodies.reserve(functions.size());
  sizes.reserve(functions.size());
  references.reserve(functions.size());

  const size_t maxFileBytes = ctx_.limits().max_file_size_bytes;
  std::unordered_set<std::string> referenced;
  std::vector<MMIOReportSite> mmio_sites;
  emitCtx.referenced = &referenced;
  emitCtx.mmio_sites = &mmio_sites;
  for (const auto* fn : functions) {
    referenced.clear();
    std::string code = fn->emitCpp(emitCtx);
    if (code.size() > maxFileBytes) {
      REXLOG_WARN(codegen, "Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})",
                  fn->base(), code.size(), maxFileBytes);
    }
    sizes.push_back({fn->base(), code.size()});
    bodies.push_back(std::move(code));
    references.push_back(referenced);
  }
  emitCtx.referenced = nullptr;
  emitCtx.mmio_sites = nullptr;

  for (const auto& [address, declared] : config().declaredMMIOAccesses) {
    const std::string_view block = declared.block == MMIOBlock::GPU ? "gpu" : "apu";
    const std::string_view list = declared.write ? "write" : "read";
    const auto site = std::ranges::find_if(mmio_sites, [address](const MMIOReportSite& entry) {
      return entry.site.address == address;
    });
    if (site == mmio_sites.end() || site->site.reason == MMIOReason::Unsupported) {
      REX_FAIL(codegen, false, "[mmio.{}] {} 0x{:08X} is not a load or store codegen routes", block,
               list, address);
    }
    if (site->site.write != declared.write) {
      REX_FAIL(codegen, false, "[mmio.{}] {} 0x{:08X} is a {}", block, list, address,
               site->site.write ? "store" : "load");
    }
    const MMIOAccess expected =
        declared.block == MMIOBlock::GPU ? MMIOAccess::GPU : MMIOAccess::APU;
    if (site->site.access != expected) {
      REX_FAIL(codegen, false, "[mmio.{}] {} 0x{:08X} reaches the {} block", block, list, address,
               BlockName(site->site.access));
    }
  }

  auto partition = OutputPartition::Load(outputPath / kPartitionFileName);
  auto buckets = partition.Assign(sizes, maxFileBytes);

  for (size_t index = 0; index < buckets.size(); ++index) {
    // Buckets are address-ordered, so a call can precede its definition.
    std::unordered_set<std::string> needed;
    for (size_t entry : buckets[index]) {
      needed.insert(references[entry].begin(), references[entry].end());
    }

    std::vector<std::string> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end());
    println("#pragma once\n");
    println("#include \"{}_pch.h\"\n", projectName);
    for (const auto& name : ordered) {
      println("DECLARE_REX_FUNC({});", name);
    }
    SaveCurrentOutData(std::format("{}_funcs.{}.h", projectName, index));

    println("#include \"{}_funcs.{}.h\"\n", projectName, index);
    for (size_t entry : buckets[index]) {
      out += bodies[entry];
    }
    SaveCurrentOutData(std::format("{}_recomp.{}.cpp", projectName, index));
  }

  out = partition.Serialize();
  SaveCurrentOutData(kPartitionFileName);

  out = BuildMMIOReport(mmio_sites).dump(2);
  SaveCurrentOutData(kMMIOReportFileName);

  REXLOG_TRACE(codegen, "Recompilation complete.");

  // Generate sources.cmake
  REXLOG_TRACE(codegen, "Recompile: generating sources.cmake");
  {
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    for (size_t i = 0; i < buckets.size(); ++i) {
      recompFiles.push_back(std::format("{}_recomp.{}.cpp", projectName, i));
    }
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  return FlushPendingWrites();
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    pendingWrites.emplace_back(std::string(name), std::move(out));
    out.clear();
  }
}

bool CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  std::unordered_set<std::string> emitted;
  emitted.reserve(pendingWrites.size());

  // A swallowed failure would be stamped as success and skipped on the next run.
  bool ok = true;

  for (const auto& [filename, content] : pendingWrites) {
    emitted.insert(filename);
    auto filePath = outputPath / filename;

    switch (WriteIfChanged(filePath, content)) {
      case WriteOutcome::Failed:
        REXLOG_ERROR(codegen, "Failed to write {}", filePath.string());
        ok = false;
        break;
      case WriteOutcome::Unchanged:
        REXLOG_TRACE(codegen, "Unchanged, skipping write: {}", filePath.string());
        unchangedFiles_.push_back(filename);
        break;
      case WriteOutcome::Written:
        REXLOG_TRACE(codegen, "Wrote {} bytes to {}", content.size(), filePath.string());
        writtenFiles_.push_back(filename);
        break;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    if (!entry.is_regular_file())
      continue;
    auto filename = entry.path().filename().string();
    if (emitted.contains(filename))
      continue;
    if (!IsGeneratedOutputName(filename, config().projectName))
      continue;

    std::error_code ec;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      REXLOG_ERROR(codegen, "Failed to delete stale output {}: {}", entry.path().string(),
                   ec.message());
      continue;
    }
    REXLOG_TRACE(codegen, "Deleted stale output: {}", entry.path().string());
    deletedFiles_.push_back(filename);
  }

  pendingWrites.clear();
  return ok;
}

}  // namespace rex::codegen
