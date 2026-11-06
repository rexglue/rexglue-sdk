#include "test_recompiler.h"
#include "../ui/ui.h"
#include "test_spec.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <CLI/CLI.hpp>
#include <format>
#include <nlohmann/json.hpp>

#include <rex/platform/file.h>
#include "codegen/binary_view.h"
#include "codegen/codegen_context.h"
#include "codegen/function_graph.h"
#include "codegen/function_scanner.h"
#include "codegen/template_registry.h"
#include "codegen/module_stub.h"
#include <rex/logging/log_category.h>
#include "rexglue/log_categories.h"
#include <rex/kernel/symbol_map.h>

#include "codegen/template_render.h"

namespace rexglue::cli {

namespace {

namespace fs = std::filesystem;
namespace codegen = rex::codegen;

constexpr u32 kTestBaseAddress = 0x82010000;

std::map<size_t, std::string> ParseMapFile(const std::string& map_path) {
  std::map<size_t, std::string> symbols;
  rex::kernel::MapParseOptions options;
  options.base_address = kTestBaseAddress;

  auto result = rex::kernel::ParseNmMap(map_path, options);
  if (!result)
    return symbols;
  for (const auto& sym : *result) {
    if (sym.name.empty() || sym.name[0] == '.')
      continue;
    symbols[sym.address] = sym.name;
  }
  return symbols;
}

nlohmann::json SerializeRegisters(const std::vector<RegValue>& regs) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& rv : regs) {
    nlohmann::json reg;
    reg["reg"] = rv.reg;
    if (rv.reg == "cr") {
      reg["type"] = "cr";
      reg["value"] = rv.value;
    } else if (rv.is_vector) {
      reg["type"] = "vector";
      reg["values"] = {rv.vec_values[3], rv.vec_values[2], rv.vec_values[1], rv.vec_values[0]};
    } else if (rv.is_float) {
      reg["type"] = "float";
      reg["value"] = rv.value;
    } else {
      reg["type"] = "gpr";
      reg["value"] = rv.value;
    }
    arr.push_back(reg);
  }
  return arr;
}

nlohmann::json SerializeMemory(const std::vector<MemValue>& mems) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& mv : mems) {
    nlohmann::json mem;
    mem["address"] = mv.address;
    nlohmann::json bytes = nlohmann::json::array();
    for (size_t i = 0; i < mv.data.size(); ++i) {
      bytes.push_back(
          {{"offset", std::format("{:X}", i)}, {"value", std::format("{:02X}", mv.data[i])}});
    }
    mem["bytes"] = bytes;
    arr.push_back(mem);
  }
  return arr;
}

std::string CategoryFromStem(std::string_view stem) {
  auto contains = [&](std::string_view s) { return stem.find(s) != std::string_view::npos; };
  if (contains("add") || contains("sub") || contains("mul") || contains("div"))
    return "arithmetic";
  if (contains("cmp"))
    return "comparison";
  if (contains("and") || contains("or") || contains("xor") || contains("rl"))
    return "logical";
  if (stem.starts_with("f") || contains("_f"))
    return "floating_point";
  if (stem.starts_with("v") || contains("_v"))
    return "vector";
  if (stem.starts_with("l") || stem.starts_with("st"))
    return "memory";
  return "misc";
}

}  // namespace

bool RecompileTests(std::string_view bin_dir, std::string_view asm_dir, std::string_view out_dir,
                    size_t case_files) {
  if (case_files == 0) {
    REXGLUE_ERROR("The number of case files must be at least 1");
    return false;
  }
  std::array<ui::KeyValueRow, 3> header_rows = {{
      {"Bin dir", std::string(bin_dir)},
      {"ASM dir", std::string(asm_dir)},
      {"Output dir", std::string(out_dir)},
  }};
  ui::KeyValueBlock("Recompiling PPC tests:", header_rows);

  std::error_code ec;
  if (!fs::is_directory(bin_dir, ec)) {
    REXGLUE_ERROR("Binary folder not found: {}", bin_dir);
    return false;
  }
  fs::create_directories(out_dir, ec);
  if (ec) {
    REXGLUE_ERROR("Cannot create output folder {}: {}", out_dir, ec.message());
    return false;
  }

  bool succeeded = true;
  size_t binary_count = 0;
  std::map<std::string, std::unordered_set<size_t>> functions_by_file;
  std::vector<std::string> all_function_names;
  std::stringstream functions_out;

  for (const auto& entry : fs::directory_iterator(bin_dir)) {
    if (entry.path().extension() != ".bin")
      continue;
    ++binary_count;
    auto stem = entry.path().stem().string();
    REXGLUE_DEBUG("Processing binary file: {}", stem);

    auto loaded = rex::platform::ReadHostFile(entry.path());
    if (!loaded) {
      REXGLUE_ERROR("Failed to load binary file: {}", entry.path().string());
      succeeded = false;
      continue;
    }
    std::vector<u8> file_data = std::move(*loaded);
    if (file_data.empty()) {
      REXGLUE_ERROR("Binary file is empty: {}", entry.path().string());
      succeeded = false;
      continue;
    }

    auto map_path = std::format("{}/{}.map", bin_dir, stem);
    auto symbols = ParseMapFile(map_path);
    if (symbols.empty()) {
      REXGLUE_ERROR("No symbols found in map file: {}", map_path);
      succeeded = false;
      continue;
    }

    codegen::StubModule module;
    module.Load(kTestBaseAddress, file_data.data(), file_data.size());
    module.SetName(stem);

    codegen::RecompilerConfig config;
    config.out_directory_path = std::string(out_dir);
    config.vector_math = {
        codegen::VectorMath::kExact, codegen::VectorMath::kExact, codegen::VectorMath::kExact,
        codegen::VectorMath::kExact, codegen::VectorMath::kExact, codegen::VectorMath::kExact,
        codegen::VectorMath::kExact, codegen::VectorMath::kExact, codegen::VectorMath::kExact};
    config.forward_stack_stores = true;
    auto ctx =
        codegen::CodegenContext::Create(codegen::BinaryView::FromModule(module), std::move(config));

    codegen::AnalyzeTestBinary(ctx, stem, symbols, kTestBaseAddress, file_data.data(),
                               file_data.size());

    REXGLUE_DEBUG("  Found {} functions", ctx.graph.GetFunctionCount());

    std::vector<const codegen::FunctionNode*> functions;
    for (const auto& [addr, node] : ctx.graph.GetFunctions())
      functions.push_back(node.get());
    std::sort(functions.begin(), functions.end(),
              [](const auto* a, const auto* b) { return a->GetBase() < b->GetBase(); });

    codegen::EmitContext emit_ctx{ctx.GetBinary(), ctx.GetConfig(), ctx.graph, ctx.GetLimits(), 0,
                                  nullptr};

    std::string recompiled_code;
    for (const auto* fn : functions) {
      std::string code = fn->EmitCpp(emit_ctx);
      if (code.empty())
        continue;
      functions_by_file[stem].emplace(fn->GetBase());
      all_function_names.push_back(std::format("{}_{:X}", stem, fn->GetBase()));
      recompiled_code += code;
    }
    functions_out << recompiled_code << '\n';
  }

  if (binary_count == 0) {
    REXGLUE_ERROR("No .bin files found in {}", bin_dir);
    return false;
  }

  std::unordered_map<std::string, std::string> all_symbols;
  for (const auto& [stem, addresses] : functions_by_file) {
    auto map_path = std::format("{}/{}.map", bin_dir, stem);
    auto map_symbols = ParseMapFile(map_path);
    for (const auto& [addr, name] : map_symbols) {
      if (addresses.count(addr))
        all_symbols.emplace(name, std::format("{}_{:X}", stem, addr));
    }
    if (!map_symbols.empty())
      continue;

    std::vector<size_t> sorted_addresses(addresses.begin(), addresses.end());
    std::sort(sorted_addresses.begin(), sorted_addresses.end());
    std::ifstream in(std::format("{}/{}.s", asm_dir, stem));
    if (!in.is_open())
      continue;
    std::string line;
    size_t function_index = 0;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line[0] == '/' || line[0] == '*')
        continue;
      auto colon_idx = line.find(':');
      if (colon_idx == std::string::npos || line[0] == ' ' || line[0] == '\t' || line[0] == '.')
        continue;
      auto name = line.substr(0, colon_idx);
      if (function_index < sorted_addresses.size()) {
        all_symbols.emplace(name, std::format("{}_{:X}", stem, sorted_addresses[function_index]));
        ++function_index;
      }
    }
  }

  nlohmann::json template_data;
  template_data["functions"] = nlohmann::json::array();
  for (const auto& func_name : all_function_names)
    template_data["functions"].push_back({{"name", func_name}});

  template_data["stems"] = nlohmann::json::array();
  for (const auto& [stem, addresses] : functions_by_file) {
    std::vector<size_t> sorted_addresses(addresses.begin(), addresses.end());
    std::sort(sorted_addresses.begin(), sorted_addresses.end());
    nlohmann::json stem_functions = nlohmann::json::array();
    for (size_t address : sorted_addresses) {
      stem_functions.push_back({{"name", std::format("{}_{:X}", stem, address)},
                                {"address", std::format("{:X}", address)}});
    }
    template_data["stems"].push_back({{"stem", stem}, {"functions", std::move(stem_functions)}});
  }
  template_data["functions_code"] = functions_out.str();
  template_data["image_base"] = "82010000";
  template_data["image_size"] = "100000";
  template_data["code_base"] = "82010000";
  template_data["code_size"] = "100000";

  template_data["tests"] = nlohmann::json::array();
  size_t total_tests = 0;
  for (const auto& [stem, addresses] : functions_by_file) {
    std::ifstream asm_file(std::format("{}/{}.s", asm_dir, stem));
    if (!asm_file.is_open()) {
      REXGLUE_WARN("Unable to open assembly file: {}/{}.s", asm_dir, stem);
    }
    auto specs = ParseTestSpecs(asm_file, all_symbols);
    if (!specs) {
      REXGLUE_ERROR("{}.s: {}", stem, specs.error().GetDescription());
      succeeded = false;
      continue;
    }
    std::string category = CategoryFromStem(stem);
    for (const auto& spec : *specs) {
      nlohmann::json test_json;
      test_json["name"] = BuildHierarchicalName(stem, spec.name);
      test_json["category"] = category;
      test_json["stem"] = stem;
      test_json["symbol"] = spec.symbol;
      test_json["inputs"]["registers"] = SerializeRegisters(spec.inputs);
      test_json["inputs"]["memory"] = SerializeMemory(spec.mem_inputs);
      test_json["outputs"]["registers"] = SerializeRegisters(spec.outputs);
      test_json["outputs"]["memory"] = SerializeMemory(spec.mem_outputs);
      template_data["tests"].push_back(test_json);
      ++total_tests;
    }
  }

  codegen::TemplateRegistry registry;
  auto write_rendered = [&](std::string_view template_id, std::string_view filename,
                            const nlohmann::json& data) {
    auto rendered = codegen::RenderWithJSON(registry, std::string(template_id), data);
    if (auto written =
            rex::platform::WriteHostFile(std::format("{}/{}", out_dir, filename), rendered);
        !written) {
      REXGLUE_ERROR("write failed: {}", written.error().GetDescription());
      succeeded = false;
    }
  };
  write_rendered("test/ppc_config_h", "ppc_config.h", template_data);
  write_rendered("test/ppc_test_decls_h", "ppc_test_decls.h", template_data);
  write_rendered("test/ppc_test_functions_cpp", "ppc_test_functions.cpp", template_data);
  for (size_t part = 0; part < case_files; ++part) {
    nlohmann::json part_data = template_data;
    part_data["tests"] = nlohmann::json::array();
    for (size_t index = part; index < template_data["tests"].size(); index += case_files)
      part_data["tests"].push_back(template_data["tests"][index]);
    part_data["define_memory"] = part == 0;
    write_rendered("test/ppc_test_cases_cpp", std::format("ppc_test_cases_{}.cpp", part),
                   part_data);
  }

  REXGLUE_TRACE("Generated {} test cases", total_tests);
  return succeeded;
}

namespace {

struct RecompileTestsOptions {
  std::string bin_dir;
  std::string asm_dir;
  std::string output;
  size_t case_files = 1;
};

}  // namespace

void RegisterRecompileTests(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  (void)ctx;
  auto args = std::make_shared<RecompileTestsOptions>();
  auto* sub = parent.add_subcommand("recompile-tests", "Generate Catch2 tests from PPC assembly")
                  ->fallthrough();
  sub->add_option("--bin-dir", args->bin_dir, "Directory containing linked .bin and .map files")
      ->type_name("PATH")
      ->required();
  sub->add_option("--asm-dir", args->asm_dir, "Directory containing .s assembly source files")
      ->type_name("PATH")
      ->required();
  sub->add_option("--output", args->output, "Output path for recompile-tests")
      ->type_name("PATH")
      ->required();
  sub->add_option("--case-files", args->case_files,
                  "Number of ppc_test_cases_N.cpp files to split the cases into")
      ->type_name("N")
      ->check(CLI::PositiveNumber);
  sub->callback([args, &pending]() {
    pending = [args]() -> rex::Result<void> {
      if (!RecompileTests(args->bin_dir, args->asm_dir, args->output, args->case_files)) {
        return Err<void>(rex::ErrorCategory::kValidation, "Test recompilation failed");
      }
      return rex::Ok();
    };
  });
}

}  // namespace rexglue::cli
