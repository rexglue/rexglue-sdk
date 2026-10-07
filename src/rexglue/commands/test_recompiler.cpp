/**
 * @file        rexglue/commands/test_recompiler.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "test_recompiler.h"
#include "../ui/ui.h"

#include <algorithm>
#include <array>
#include <charconv>
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

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/template_registry.h>
#include <rex/codegen/test_support.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/runtime/map_parser.h>

#include "codegen/template_registry_internal.h"

namespace rexglue::cli {

namespace {

namespace fs = std::filesystem;
namespace codegen = rex::codegen;

constexpr uint32_t kTestBaseAddress = 0x82010000;

struct RegValue {
  std::string reg;
  std::string value;
  bool is_vector = false;
  bool is_float = false;
  std::string vec_values[4];
};

struct MemValue {
  std::string address;
  std::vector<uint8_t> data;
};

struct TestSpec {
  std::string name;
  std::string symbol;
  std::vector<RegValue> inputs;
  std::vector<RegValue> outputs;
  std::vector<MemValue> mem_inputs;
  std::vector<MemValue> mem_outputs;
};

std::map<size_t, std::string> ParseMapFile(const std::string& mapPath) {
  std::map<size_t, std::string> symbols;
  rex::runtime::MapParseOptions options;
  options.base_address = kTestBaseAddress;

  auto result = rex::runtime::ParseNmMap(mapPath, options);
  if (!result)
    return symbols;
  for (const auto& sym : *result) {
    if (sym.name.empty() || sym.name[0] == '.')
      continue;
    symbols[sym.address] = sym.name;
  }
  return symbols;
}

std::vector<uint8_t> ParseHexBytes(const std::string& hex) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < hex.size(); i += 2) {
    if (i + 1 >= hex.size())
      break;
    if (hex[i] == ' ') {
      i--;
      continue;
    }
    uint8_t val = 0;
    std::from_chars(&hex[i], &hex[i + 2], val, 16);
    result.push_back(val);
  }
  return result;
}

/* Source order is high-to-low; stored low-to-high to match runtime layout. */
RegValue ParseVectorRegister(const std::string& line, std::string reg, size_t after) {
  RegValue rv;
  rv.reg = std::move(reg);
  rv.is_vector = true;
  auto open = line.find('[', after + 1);
  auto c0 = line.find(',', open + 1);
  auto c1 = line.find(',', c0 + 1);
  auto c2 = line.find(',', c1 + 1);
  auto close = line.find(']', c2 + 1);
  rv.vec_values[3] = line.substr(open + 1, c0 - open - 1);
  rv.vec_values[2] = line.substr(c0 + 2, c1 - c0 - 2);
  rv.vec_values[1] = line.substr(c1 + 2, c2 - c1 - 2);
  rv.vec_values[0] = line.substr(c2 + 2, close - c2 - 2);
  return rv;
}

RegValue ParseRegisterDirective(const std::string& line, size_t directive_idx) {
  auto sp1 = line.find(' ', directive_idx);
  auto sp2 = line.find(' ', sp1 + 1);
  std::string reg = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (!reg.empty() && reg[0] == 'v')
    return ParseVectorRegister(line, std::move(reg), sp2);

  RegValue rv;
  rv.reg = std::move(reg);
  rv.value = line.substr(sp2 + 1);
  rv.is_float = (line.find('.', sp2) != std::string::npos);
  return rv;
}

MemValue ParseMemoryDirective(const std::string& line, size_t directive_idx) {
  auto sp1 = line.find(' ', directive_idx);
  auto sp2 = line.find(' ', sp1 + 1);
  MemValue mv;
  mv.address = line.substr(sp1 + 1, sp2 - sp1 - 1);
  mv.data = ParseHexBytes(line.substr(sp2 + 1));
  return mv;
}

void ApplyDirective(const std::string& line, std::string_view reg_token, std::string_view mem_token,
                    std::vector<RegValue>& regs, std::vector<MemValue>& mems) {
  if (line.size() <= 1 || line[1] != '_')
    return;
  if (auto idx = line.find(reg_token); idx != std::string::npos) {
    regs.push_back(ParseRegisterDirective(line, idx));
  } else if (auto idx2 = line.find(mem_token); idx2 != std::string::npos) {
    mems.push_back(ParseMemoryDirective(line, idx2));
  }
}

std::string BuildHierarchicalName(const std::string& stem, const std::string& label) {
  if (stem.starts_with("instr_")) {
    std::string group = stem.substr(6);
    std::string prefix = "test_" + group + "_";
    if (label.starts_with(prefix))
      return group + ".test_" + label.substr(prefix.size());
    if (label == "test_" + group)
      return group + ".test";
    return group + "." + label;
  }
  if (stem.starts_with("seq_")) {
    if (label.starts_with("seq_"))
      return "seq.test_" + label.substr(4);
    return "seq." + label;
  }
  return stem + "." + label;
}

std::vector<TestSpec> ParseTestSpecs(const std::string& asmPath,
                                     const std::unordered_map<std::string, std::string>& symbols) {
  std::vector<TestSpec> specs;
  std::ifstream in(asmPath);
  if (!in.is_open()) {
    REXLOG_WARN(rexglue, "Unable to open assembly file: {}", asmPath);
    return specs;
  }

  std::string line;
  bool in_block_comment = false;
  auto getline = [&]() -> bool {
    while (std::getline(in, line)) {
      line = rex::trim_string(line);
      if (in_block_comment) {
        auto end = line.find("*/");
        if (end == std::string::npos)
          continue;
        in_block_comment = false;
        line = rex::trim_string(line.substr(end + 2));
        if (line.empty())
          continue;
      }
      if (auto start = line.find("/*"); start != std::string::npos) {
        if (auto end = line.find("*/", start + 2); end != std::string::npos) {
          line = line.substr(0, start) + line.substr(end + 2);
        } else {
          in_block_comment = true;
          line = line.substr(0, start);
        }
        line = rex::trim_string(line);
        if (line.empty())
          continue;
      }
      return true;
    }
    return false;
  };

  while (getline()) {
    if (line.empty() || line[0] == '#')
      continue;
    auto colonIndex = line.find(':');
    if (colonIndex == std::string::npos)
      continue;
    auto name = line.substr(0, colonIndex);
    auto symbolIt = symbols.find(name);
    if (symbolIt == symbols.end())
      continue;

    TestSpec spec;
    spec.name = name;
    spec.symbol = symbolIt->second;

    while (getline() && !line.empty() && line[0] == '#') {
      ApplyDirective(line, "REGISTER_IN", "MEMORY_IN", spec.inputs, spec.mem_inputs);
    }
    while (line.empty() || line[0] != '#') {
      if (!getline())
        break;
    }
    do {
      ApplyDirective(line, "REGISTER_OUT", "MEMORY_OUT", spec.outputs, spec.mem_outputs);
    } while (getline() && !line.empty() && line[0] == '#');

    if (!spec.inputs.empty() || !spec.outputs.empty() || !spec.mem_inputs.empty() ||
        !spec.mem_outputs.empty()) {
      specs.push_back(std::move(spec));
    }
  }
  return specs;
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

bool RecompileTests(std::string_view binDir, std::string_view asmDir, std::string_view outDir) {
  std::array<ui::KeyValueRow, 3> header_rows = {{
      {"Bin dir", std::string(binDir)},
      {"ASM dir", std::string(asmDir)},
      {"Output dir", std::string(outDir)},
  }};
  ui::KeyValueBlock("Recompiling PPC tests:", header_rows);

  fs::create_directories(outDir);

  std::map<std::string, std::unordered_set<size_t>> functionsByFile;
  std::vector<std::string> allFunctionNames;
  std::stringstream functionsOut;

  for (const auto& entry : fs::directory_iterator(binDir)) {
    if (entry.path().extension() != ".bin")
      continue;
    auto stem = entry.path().stem().string();
    REXLOG_DEBUG(rexglue, "Processing binary file: {}", stem);

    std::vector<uint8_t> fileData;
    {
      std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
      if (!file) {
        REXLOG_WARN(rexglue, "Failed to load binary file: {}", entry.path().string());
        continue;
      }
      auto size = file.tellg();
      file.seekg(0, std::ios::beg);
      fileData.resize(static_cast<size_t>(size));
      file.read(reinterpret_cast<char*>(fileData.data()), size);
    }
    if (fileData.empty())
      continue;

    auto mapPath = std::format("{}/{}.map", binDir, stem);
    auto symbols = ParseMapFile(mapPath);
    if (symbols.empty()) {
      REXLOG_ERROR(rexglue, "No symbols found in map file: {}", mapPath);
      continue;
    }

    codegen::TestModule module;
    module.Load(kTestBaseAddress, fileData.data(), fileData.size());
    module.set_name(stem);

    codegen::RecompilerConfig config;
    config.outDirectoryPath = std::string(outDir);
    auto ctx =
        codegen::CodegenContext::Create(codegen::BinaryView::fromModule(module), std::move(config));

    codegen::AnalyzeTestBinary(ctx, stem, symbols, kTestBaseAddress, fileData.data(),
                               fileData.size());

    REXLOG_DEBUG(rexglue, "  Found {} functions", ctx.graph.functionCount());

    std::vector<const codegen::FunctionNode*> functions;
    for (const auto& [addr, node] : ctx.graph.functions())
      functions.push_back(node.get());
    std::sort(functions.begin(), functions.end(),
              [](const auto* a, const auto* b) { return a->base() < b->base(); });

    codegen::EmitContext emitCtx{ctx.binary(), ctx.Config(), ctx.graph, ctx.limits(), 0, nullptr};

    std::string recompiledCode;
    for (const auto* fn : functions) {
      std::string code = fn->emitCpp(emitCtx);
      if (code.empty())
        continue;
      functionsByFile[stem].emplace(fn->base());
      allFunctionNames.push_back(std::format("{}_{:X}", stem, fn->base()));
      recompiledCode += code;
    }
    functionsOut << recompiledCode << '\n';
  }

  std::unordered_map<std::string, std::string> allSymbols;
  for (const auto& [stem, addresses] : functionsByFile) {
    auto mapPath = std::format("{}/{}.map", binDir, stem);
    auto mapSymbols = ParseMapFile(mapPath);
    for (const auto& [addr, name] : mapSymbols) {
      if (addresses.count(addr))
        allSymbols.emplace(name, std::format("{}_{:X}", stem, addr));
    }
    if (!mapSymbols.empty())
      continue;

    std::vector<size_t> sortedAddresses(addresses.begin(), addresses.end());
    std::sort(sortedAddresses.begin(), sortedAddresses.end());
    std::ifstream in(std::format("{}/{}.s", asmDir, stem));
    if (!in.is_open())
      continue;
    std::string line;
    size_t functionIndex = 0;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line[0] == '/' || line[0] == '*')
        continue;
      auto colonIdx = line.find(':');
      if (colonIdx == std::string::npos || line[0] == ' ' || line[0] == '\t' || line[0] == '.')
        continue;
      auto name = line.substr(0, colonIdx);
      if (functionIndex < sortedAddresses.size()) {
        allSymbols.emplace(name, std::format("{}_{:X}", stem, sortedAddresses[functionIndex]));
        ++functionIndex;
      }
    }
  }

  nlohmann::json templateData;
  templateData["functions"] = nlohmann::json::array();
  for (const auto& funcName : allFunctionNames)
    templateData["functions"].push_back({{"name", funcName}});
  templateData["functions_code"] = functionsOut.str();
  templateData["image_base"] = "82010000";
  templateData["image_size"] = "100000";
  templateData["code_base"] = "82010000";
  templateData["code_size"] = "100000";

  templateData["tests"] = nlohmann::json::array();
  size_t totalTests = 0;
  for (const auto& [stem, addresses] : functionsByFile) {
    auto specs = ParseTestSpecs(std::format("{}/{}.s", asmDir, stem), allSymbols);
    std::string category = CategoryFromStem(stem);
    for (const auto& spec : specs) {
      nlohmann::json testJson;
      testJson["name"] = BuildHierarchicalName(stem, spec.name);
      testJson["category"] = category;
      testJson["stem"] = stem;
      testJson["symbol"] = spec.symbol;
      testJson["inputs"]["registers"] = SerializeRegisters(spec.inputs);
      testJson["inputs"]["memory"] = SerializeMemory(spec.mem_inputs);
      testJson["outputs"]["registers"] = SerializeRegisters(spec.outputs);
      testJson["outputs"]["memory"] = SerializeMemory(spec.mem_outputs);
      templateData["tests"].push_back(testJson);
      ++totalTests;
    }
  }

  codegen::TemplateRegistry registry;
  auto writeRendered = [&](std::string_view templateId, std::string_view filename) {
    auto rendered = codegen::renderWithJson(registry, std::string(templateId), templateData);
    std::ofstream out(std::format("{}/{}", outDir, filename));
    out << rendered;
  };
  writeRendered("test/ppc_config_h", "ppc_config.h");
  writeRendered("test/ppc_test_decls_h", "ppc_test_decls.h");
  writeRendered("test/ppc_test_functions_cpp", "ppc_test_functions.cpp");
  writeRendered("test/ppc_test_cases_cpp", "ppc_test_cases.cpp");

  REXLOG_TRACE(rexglue, "Generated {} test cases", totalTests);
  return true;
}

struct RecompileTestsArgs {
  std::string bin_dir;
  std::string asm_dir;
  std::string output;
};

}  // namespace

void RegisterRecompileTests(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending) {
  (void)ctx;
  auto args = std::make_shared<RecompileTestsArgs>();
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
  sub->callback([args, &pending]() {
    pending = [args]() -> rex::Result<void> {
      if (!RecompileTests(args->bin_dir, args->asm_dir, args->output)) {
        return Err<void>(rex::ErrorCategory::Validation, "Test recompilation failed");
      }
      return rex::Ok();
    };
  });
}

}  // namespace rexglue::cli
