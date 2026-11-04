#include "codegen_writer.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <format>
#include <inja/inja.hpp>

#include "call_usage.h"
#include "function_graph.h"
#include "output_partition.h"
#include "template_registry.h"
#include <rex/logging/log_category.h>
#include "codegen_categories.h"

#include "file_io.h"
#include "mmio_analysis.h"
#include "template_render.h"

namespace {

nlohmann::json BuildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<u32, std::string>& shim_by_addr) {
  const auto& cfg = ctx.GetConfig();

  size_t code_min = ~static_cast<size_t>(0);
  size_t code_max = 0;
  for (const auto& section : ctx.GetBinary().GetSections()) {
    if (section.executable) {
      if (section.base_address < code_min)
        code_min = section.base_address;
      if ((section.base_address + section.size) > code_max)
        code_max = section.base_address + section.size;
    }
  }

  nlohmann::json functions_json = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string func_name;

    auto shim_it = shim_by_addr.find(fn->GetBase());
    if (shim_it != shim_by_addr.end()) {
      func_name = shim_it->second;
    } else if (fn->GetBase() == ctx.GetAnalysisState().entry_point) {
      func_name = "xstart";
    } else if (!fn->GetName().empty()) {
      func_name = fn->GetName();
    } else {
      func_name = std::format("sub_{:08X}", fn->GetBase());
    }

    const auto& module_imports = ctx.GetAnalysisState().module_imports;
    const auto module_import = module_imports.find(fn->GetBase());
    const bool is_module_import = module_import != module_imports.end();
    functions_json.push_back({
        {"address", std::format("0x{:X}", fn->GetBase())},
        {"name", func_name},
        {"below_code_base", (fn->GetBase() < code_min)},
        {"is_import", fn->GetAuthority() == rex::codegen::FunctionAuthority::kImport},
        {"is_module_import", is_module_import},
        {"import_library", is_module_import ? module_import->second.library : std::string()},
        {"import_ordinal", is_module_import ? module_import->second.ordinal : 0},
    });
  }

  std::unordered_set<u32> millicode_entries;
  for (const auto* fn : functions) {
    if (fn->GetMillicode()) {
      millicode_entries.insert(fn->GetBase());
    }
  }

  std::vector<const rex::codegen::FunctionNode*> analysed;
  for (const auto* fn : functions) {
    if (fn->GetAuthority() != rex::codegen::FunctionAuthority::kImport &&
        fn->GetBase() >= code_min) {
      analysed.push_back(fn);
    }
  }
  const auto usages = rex::codegen::ComputeCallUsages(analysed, millicode_entries);
  std::vector<std::pair<u32, rex::codegen::CallUsage>> usage_entries(usages.begin(), usages.end());
  std::ranges::sort(usage_entries, {}, &decltype(usage_entries)::value_type::first);
  nlohmann::json call_usage_json = nlohmann::json::array();
  for (const auto& [address, usage] : usage_entries) {
    call_usage_json.push_back({
        {"address", std::format("0x{:X}", address)},
        {"gpr_mask", std::format("0x{:X}", usage.gpr_mask)},
        {"fpr_mask", std::format("0x{:X}", usage.fpr_mask)},
    });
  }

  nlohmann::json config_flags = {
      {"skip_lr", cfg.skip_lr},
      {"ctr_as_local", cfg.ctr_as_local_variable},
      {"xer_as_local", cfg.xer_as_local_variable},
      {"reserved_as_local", cfg.reserved_register_as_local_variable},
      {"skip_msr", cfg.skip_msr},
      {"cr_as_local", cfg.cr_registers_as_local_variables},
      {"non_argument_as_local", cfg.non_argument_registers_as_local_variables},
      {"non_volatile_as_local", cfg.non_volatile_registers_as_local_variables},
  };

  return {
      {"project", cfg.project_name},
      {"image_base", std::format("0x{:X}", ctx.GetBinary().GetBaseAddress())},
      {"image_size", std::format("0x{:X}", ctx.GetBinary().GetImageSize())},
      {"code_base", std::format("0x{:X}", code_min)},
      {"code_size", std::format("0x{:X}", code_max - code_min)},
      {"shim_heap", cfg.UsesShimHeap() ? 1 : 0},
      {"thunk_reserve_size", std::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.HasDLLModules()},
      {"is_dll", ctx.IsDLLModule()},
      {"config_flags", config_flags},
      {"functions", functions_json},
      {"call_usage", call_usage_json},
      {"recomp_files", nlohmann::json::array()},
  };
}

}  // namespace

namespace rex::codegen {

namespace {

bool IsDigits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

bool IsPrefixedGeneratedName(std::string_view filename) {
  constexpr std::array<std::string_view, 5> kSuffixes{"_pch.h", "_funcs.h", "_init.h", "_init.cpp",
                                                      "_register.cpp"};
  for (auto suffix : kSuffixes) {
    if (filename.size() > suffix.size() && filename.ends_with(suffix))
      return true;
  }
  constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kIndexed{
      {{"_funcs.", ".h"}, {"_recomp.", ".cpp"}}};
  for (auto [stem, ext] : kIndexed) {
    if (!filename.ends_with(ext))
      continue;
    const auto at = filename.rfind(stem);
    if (at == std::string_view::npos || at == 0)
      continue;
    const auto index =
        filename.substr(at + stem.size(), filename.size() - at - stem.size() - ext.size());
    if (IsDigits(index))
      return true;
  }
  return false;
}

bool IsGeneratedOutputName(std::string_view filename, std::string_view project_name) {
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

  std::string project_prefix(project_name);
  project_prefix += '_';
  return filename.starts_with(project_prefix) || IsPrefixedGeneratedName(filename);
}

}  // namespace

constexpr size_t kOutputBufferReserveSize = size_t{32} * 1024 * 1024;

namespace {

std::string HexAddress(u32 address) {
  return std::format("0x{:08X}", address);
}

std::string_view BlockName(MMIOAccess access) {
  return access == MMIOAccess::kGPU ? "GPU" : "APU";
}

nlohmann::json BuildMMIOReport(const std::vector<MMIOReportSite>& sites) {
  nlohmann::json report{{"matched", nlohmann::json::array()},
                        {"declared", nlohmann::json::array()},
                        {"guarded", nlohmann::json::array()},
                        {"unserved", nlohmann::json::array()},
                        {"unsupported", nlohmann::json::array()}};
  for (const MMIOReportSite& entry : sites) {
    nlohmann::json row{{"function", HexAddress(entry.function)},
                       {"address", HexAddress(entry.site.address)},
                       {"access", entry.site.write ? "write" : "read"}};
    switch (entry.site.reason) {
      case MMIOReason::kMatched:
        row["block"] = BlockName(entry.site.access);
        row["target"] = HexAddress(entry.site.target);
        report["matched"].push_back(std::move(row));
        break;
      case MMIOReason::kOffset:
        row["reason"] = "offset";
        report["guarded"].push_back(std::move(row));
        break;
      case MMIOReason::kBarrier:
        row["reason"] = "eieio";
        report["guarded"].push_back(std::move(row));
        break;
      case MMIOReason::kDeclared:
        row["block"] = BlockName(entry.site.access);
        report["declared"].push_back(std::move(row));
        break;
      case MMIOReason::kUnserved:
        row["target"] = HexAddress(entry.site.target);
        report["unserved"].push_back(std::move(row));
        break;
      case MMIOReason::kUnsupported:
        row["target"] = HexAddress(entry.site.target);
        report["unsupported"].push_back(std::move(row));
        break;
    }
  }
  return report;
}

}  // namespace

CodegenWriter::CodegenWriter(CodegenContext& ctx) : ctx_(ctx) {}

FunctionGraph& CodegenWriter::GetGraph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::GetGraph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::GetBinary() const {
  return ctx_.GetBinary();
}
RecompilerConfig& CodegenWriter::GetConfig() {
  return ctx_.GetConfig();
}
const RecompilerConfig& CodegenWriter::GetConfig() const {
  return ctx_.GetConfig();
}
AnalysisState& CodegenWriter::GetAnalysisState() {
  return ctx_.GetAnalysisState();
}
const AnalysisState& CodegenWriter::GetAnalysisState() const {
  return ctx_.GetAnalysisState();
}

bool CodegenWriter::Write(bool force) {
  deleted_files_.clear();
  written_files_.clear();
  unchanged_files_.clear();

  if (ctx_.errors.HasErrors() && !force) {
    REXCG_FAIL(false, "Code generation blocked: {} validation errors. Use --force to override.",
               ctx_.errors.Count());
  }

  std::filesystem::path output_path = ctx_.GetConfigDir() / GetConfig().out_directory_path;
  REXCG_TRACE("Output path: {}", output_path.string());
  std::filesystem::create_directories(output_path);

  REXCG_TRACE("Recompile: starting");
  out_.reserve(kOutputBufferReserveSize);

  std::vector<const FunctionNode*> functions;
  functions.reserve(GetGraph().GetFunctionCount());
  for (const auto& [addr, node] : GetGraph().GetFunctions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->GetBase() < b->GetBase(); });

  std::unordered_map<u32, std::string> shim_by_addr;
  for (const auto& [name, addr] : GetConfig().shim_functions) {
    auto shim_name = std::format("rexshim_{}", name);
    shim_by_addr[addr] = shim_name;
    if (auto* node = GetGraph().GetFunction(addr)) {
      node->SetName(std::move(shim_name));
    } else {
      REXCG_WARN("[shim] {} at 0x{:08X} has no function, skipped", name, addr);
    }
  }

  const std::string& project_name = GetConfig().project_name;

  TemplateRegistry registry;
  if (!GetConfig().template_dir.empty())
    registry.LoadOverrides(ctx_.GetConfigDir() / GetConfig().template_dir);

  auto tmpl_data = BuildTemplateData(ctx_, functions, shim_by_addr);

  REXCG_TRACE("Recompile: generating {}_pch.h", project_name);
  out_ = RenderWithJSON(registry, "codegen/pch_h", tmpl_data);
  QueueOutput(std::format("{}_pch.h", project_name));

  REXCG_TRACE("Recompile: generating {}_funcs.h", project_name);
  out_ = RenderWithJSON(registry, "codegen/funcs_h", tmpl_data);
  QueueOutput(std::format("{}_funcs.h", project_name));

  REXCG_TRACE("Recompile: generating {}_init.h", project_name);
  out_ = RenderWithJSON(registry, "codegen/init_h", tmpl_data);
  QueueOutput(std::format("{}_init.h", project_name));

  REXCG_TRACE("Recompile: generating {}_init.cpp", project_name);
  out_ = RenderWithJSON(registry, "codegen/init_cpp", tmpl_data);
  QueueOutput(std::format("{}_init.cpp", project_name));

  REXCG_TRACE("Recompile: generating {}_register.cpp", project_name);
  tmpl_data["is_dll"] = ctx_.IsDLLModule();
  out_ = RenderWithJSON(registry, "codegen/register_cpp", tmpl_data);
  QueueOutput(std::format("{}_register.cpp", project_name));

  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->GetAuthority() == FunctionAuthority::kImport;
  });
  std::erase_if(functions, [&shim_by_addr](const FunctionNode* fn) {
    return shim_by_addr.contains(fn->GetBase());
  });

  EmitContext emit_ctx{GetBinary(), GetConfig(), GetGraph(), ctx_.GetLimits(),
                       static_cast<u32>(GetAnalysisState().entry_point)};

  REXCG_TRACE("Recompiling {} functions...", functions.size());

  std::vector<std::string> bodies;
  std::vector<FunctionSize> sizes;
  std::vector<std::unordered_set<std::string>> references;
  bodies.reserve(functions.size());
  sizes.reserve(functions.size());
  references.reserve(functions.size());

  const size_t max_file_bytes = ctx_.GetLimits().max_file_size_bytes;
  std::unordered_set<std::string> referenced;
  std::vector<MMIOReportSite> mmio_sites;
  emit_ctx.referenced = &referenced;
  emit_ctx.mmio_sites = &mmio_sites;
  for (const auto* fn : functions) {
    referenced.clear();
    std::string code = fn->EmitCpp(emit_ctx);
    if (code.size() > max_file_bytes) {
      REXCG_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})", fn->GetBase(),
                 code.size(), max_file_bytes);
    }
    sizes.push_back({fn->GetBase(), code.size()});
    bodies.push_back(std::move(code));
    references.push_back(referenced);
  }
  emit_ctx.referenced = nullptr;
  emit_ctx.mmio_sites = nullptr;

  for (const auto& [address, declared] : GetConfig().declared_mmio_accesses) {
    const std::string_view block = declared.block == rex::ppc::MMIOBlock::kGPU ? "gpu" : "apu";
    const std::string_view list = declared.write ? "write" : "read";
    const auto site = std::ranges::find_if(mmio_sites, [address](const MMIOReportSite& entry) {
      return entry.site.address == address;
    });
    if (site == mmio_sites.end() || site->site.reason == MMIOReason::kUnsupported) {
      REXCG_FAIL(false, "[mmio.{}] {} 0x{:08X} is not a load or store codegen routes", block, list,
                 address);
    }
    if (site->site.write != declared.write) {
      REXCG_FAIL(false, "[mmio.{}] {} 0x{:08X} is a {}", block, list, address,
                 site->site.write ? "store" : "load");
    }
    const MMIOAccess expected =
        declared.block == rex::ppc::MMIOBlock::kGPU ? MMIOAccess::kGPU : MMIOAccess::kAPU;
    if (site->site.access != expected) {
      REXCG_FAIL(false, "[mmio.{}] {} 0x{:08X} reaches the {} block", block, list, address,
                 BlockName(site->site.access));
    }
  }

  auto partition = OutputPartition::Load(output_path / kPartitionFileName);
  auto buckets = partition.Assign(sizes, max_file_bytes);

  for (size_t index = 0; index < buckets.size(); ++index) {
    // Buckets are address ordered, so a call can precede its definition.
    std::unordered_set<std::string> needed;
    for (size_t entry : buckets[index]) {
      needed.insert(references[entry].begin(), references[entry].end());
    }

    std::vector<std::string> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end());
    Println("#pragma once\n");
    Println("#include \"{}_pch.h\"\n", project_name);
    for (const auto& name : ordered) {
      Println("DECLARE_REX_FUNC({});", name);
    }
    QueueOutput(std::format("{}_funcs.{}.h", project_name, index));

    Println("#include \"{}_funcs.{}.h\"\n", project_name, index);
    for (size_t entry : buckets[index]) {
      out_ += bodies[entry];
    }
    QueueOutput(std::format("{}_recomp.{}.cpp", project_name, index));
  }

  out_ = partition.Serialize();
  QueueOutput(kPartitionFileName);

  out_ = BuildMMIOReport(mmio_sites).dump(2);
  QueueOutput(kMMIOReportFileName);

  REXCG_TRACE("Recompilation complete.");

  REXCG_TRACE("Recompile: generating sources.cmake");
  {
    auto& recomp_files = tmpl_data["recomp_files"];
    recomp_files = nlohmann::json::array();
    for (size_t i = 0; i < buckets.size(); ++i) {
      recomp_files.push_back(std::format("{}_recomp.{}.cpp", project_name, i));
    }
    out_ = RenderWithJSON(registry, "codegen/sources_cmake", tmpl_data);
    QueueOutput("sources.cmake");
  }

  return FlushPendingWrites();
}

void CodegenWriter::QueueOutput(const std::string_view name) {
  if (!out_.empty()) {
    pending_writes_.emplace_back(std::string(name), std::move(out_));
    out_.clear();
  }
}

bool CodegenWriter::FlushPendingWrites() {
  std::filesystem::path output_path = ctx_.GetConfigDir() / GetConfig().out_directory_path;

  std::unordered_set<std::string> emitted;
  emitted.reserve(pending_writes_.size());

  // A swallowed failure would be stamped as success and skipped on the next run.
  bool ok = true;

  for (const auto& [filename, content] : pending_writes_) {
    emitted.insert(filename);
    auto file_path = output_path / filename;

    switch (WriteIfChanged(file_path, content)) {
      case WriteOutcome::kFailed:
        REXCG_ERROR("Failed to write {}", file_path.string());
        ok = false;
        break;
      case WriteOutcome::kUnchanged:
        REXCG_TRACE("Unchanged, skipping write: {}", file_path.string());
        unchanged_files_.push_back(filename);
        break;
      case WriteOutcome::kWritten:
        REXCG_TRACE("Wrote {} bytes to {}", content.size(), file_path.string());
        written_files_.push_back(filename);
        break;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(output_path)) {
    if (!entry.is_regular_file())
      continue;
    auto filename = entry.path().filename().string();
    if (emitted.contains(filename))
      continue;
    if (!IsGeneratedOutputName(filename, GetConfig().project_name))
      continue;

    std::error_code ec;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      REXCG_ERROR("Failed to delete stale output {}: {}", entry.path().string(), ec.message());
      continue;
    }
    REXCG_TRACE("Deleted stale output: {}", entry.path().string());
    deleted_files_.push_back(filename);
  }

  pending_writes_.clear();
  return ok;
}

}  // namespace rex::codegen
