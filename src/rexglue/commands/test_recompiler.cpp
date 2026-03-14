/**
 * @file        rexglue/commands/test_recompiler.cpp
 * @brief       Recompiler test command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "test_recompiler.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>
#include <rex/logging.h>
#include <rex/runtime.h>  // For rex::Runtime complete type
#include <rex/string.h>
#include <rex/system/map_parser.h>
#include <rex/system/module.h>  // For Module interface

namespace fs = std::filesystem;

namespace rexglue::commands {

namespace codegen = rex::codegen;

// TODO(tomc): Move test framework to its own library?
namespace {

/// Base address for linked test binaries (matches -Ttext in CMakeLists.txt)
constexpr uint32_t TEST_BASE_ADDRESS = 0x82010000;

/// Parse nm-generated map file using the runtime library parser
/// Returns map of address -> symbol name
std::map<size_t, std::string> parse_map_file(const std::string& mapPath) {
  std::map<size_t, std::string> symbols;

  rex::runtime::MapParseOptions options;
  options.base_address = TEST_BASE_ADDRESS;

  auto result = rex::runtime::ParseNmMap(mapPath, options);
  if (!result) {
    return symbols;
  }

  for (const auto& sym : *result) {
    // Skip local labels starting with .
    if (sym.name.empty() || sym.name[0] == '.')
      continue;
    symbols[sym.address] = sym.name;
  }
  return symbols;
}

/// Parse a test assembly file and extract test specifications
struct TestSpec {
  std::string name;
  std::string symbol;  // Recompiled function name

  struct RegValue {
    std::string reg;
    std::string value;
    bool is_vector = false;
    bool is_float = false;
    std::string vec_values[4];  // For vector registers
  };

  struct MemValue {
    std::string address;
    std::vector<uint8_t> data;
  };

  std::vector<RegValue> inputs;
  std::vector<RegValue> outputs;
  std::vector<MemValue> mem_inputs;
  std::vector<MemValue> mem_outputs;
};

/// Parse hex string into byte vector
std::vector<uint8_t> parse_hex_string(const std::string& hex) {
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

/// Parse test specifications from assembly file
std::vector<TestSpec> parse_test_specs(
    const std::string& asmPath, const std::unordered_map<std::string, std::string>& symbols) {
  std::vector<TestSpec> specs;
  std::ifstream in(asmPath);
  if (!in.is_open()) {
    REXLOG_WARN("Unable to open assembly file: {}", asmPath);
    return specs;
  }

  std::string line;
  bool in_block_comment = false;  // Track /* */ block comments
  auto getline = [&]() -> bool {
    while (std::getline(in, line)) {
      // Trim all whitespace (including \r for CRLF files)
      line = rex::string::trim_string(line);

      // Handle block comments
      if (in_block_comment) {
        auto endComment = line.find("*/");
        if (endComment != std::string::npos) {
          in_block_comment = false;
          line = rex::string::trim_string(line.substr(endComment + 2));
          if (line.empty())
            continue;
        } else {
          continue;  // Skip entire line inside block comment
        }
      }

      // Check for start of block comment
      auto startComment = line.find("/*");
      if (startComment != std::string::npos) {
        auto endComment = line.find("*/", startComment + 2);
        if (endComment != std::string::npos) {
          // Single-line block comment
          line = line.substr(0, startComment) + line.substr(endComment + 2);
        } else {
          // Multi-line block comment starts
          in_block_comment = true;
          line = line.substr(0, startComment);
        }
        line = rex::string::trim_string(line);
        if (line.empty())
          continue;
      }

      return true;
    }
    return false;
  };

  while (getline()) {
    // Look for function labels (name:)
    if (!line.empty() && line[0] != '#') {
      auto colonIndex = line.find(':');
      if (colonIndex != std::string::npos) {
        auto name = line.substr(0, colonIndex);
        auto symbolIt = symbols.find(name);
        if (symbolIt == symbols.end()) {
          continue;  // No symbol for this function
        }

        TestSpec spec;
        spec.name = name;
        spec.symbol = symbolIt->second;

        // Parse REGISTER_IN and MEMORY_IN directives
        while (getline() && !line.empty() && line[0] == '#') {
          if (line.size() > 1 && line[1] == '_') {
            auto registerInIndex = line.find("REGISTER_IN");
            if (registerInIndex != std::string::npos) {
              auto spaceIndex = line.find(' ', registerInIndex);
              auto secondSpaceIndex = line.find(' ', spaceIndex + 1);
              auto reg = line.substr(spaceIndex + 1, secondSpaceIndex - spaceIndex - 1);

              TestSpec::RegValue rv;
              rv.reg = reg;

              if (reg[0] == 'v') {
                // Vector register: [val0, val1, val2, val3]
                rv.is_vector = true;
                auto openBracket = line.find('[', secondSpaceIndex + 1);
                auto comma0 = line.find(',', openBracket + 1);
                auto comma1 = line.find(',', comma0 + 1);
                auto comma2 = line.find(',', comma1 + 1);
                auto closeBracket = line.find(']', comma2 + 1);

                rv.vec_values[3] = line.substr(openBracket + 1, comma0 - openBracket - 1);
                rv.vec_values[2] = line.substr(comma0 + 2, comma1 - comma0 - 2);
                rv.vec_values[1] = line.substr(comma1 + 2, comma2 - comma1 - 2);
                rv.vec_values[0] = line.substr(comma2 + 2, closeBracket - comma2 - 2);
              } else {
                rv.value = line.substr(secondSpaceIndex + 1);
                rv.is_float = (line.find('.', secondSpaceIndex) != std::string::npos);
              }
              spec.inputs.push_back(rv);
            } else {
              auto memoryInIndex = line.find("MEMORY_IN");
              if (memoryInIndex != std::string::npos) {
                auto spaceIndex = line.find(' ', memoryInIndex);
                auto secondSpaceIndex = line.find(' ', spaceIndex + 1);

                TestSpec::MemValue mv;
                mv.address = line.substr(spaceIndex + 1, secondSpaceIndex - spaceIndex - 1);
                mv.data = parse_hex_string(line.substr(secondSpaceIndex + 1));
                spec.mem_inputs.push_back(mv);
              }
            }
          }
        }

        // Skip until we find REGISTER_OUT or MEMORY_OUT
        // Note: Continue past empty lines (blank lines between instructions)
        while (line.empty() || line[0] != '#') {
          if (!getline())
            break;
        }

        // Parse REGISTER_OUT and MEMORY_OUT directives
        do {
          if (line.size() > 1 && line[1] == '_') {
            auto registerOutIndex = line.find("REGISTER_OUT");
            if (registerOutIndex != std::string::npos) {
              auto spaceIndex = line.find(' ', registerOutIndex);
              auto secondSpaceIndex = line.find(' ', spaceIndex + 1);
              auto reg = line.substr(spaceIndex + 1, secondSpaceIndex - spaceIndex - 1);

              TestSpec::RegValue rv;
              rv.reg = reg;

              if (reg[0] == 'v') {
                // Vector register
                rv.is_vector = true;
                auto openBracket = line.find('[', secondSpaceIndex + 1);
                auto comma0 = line.find(',', openBracket + 1);
                auto comma1 = line.find(',', comma0 + 1);
                auto comma2 = line.find(',', comma1 + 1);
                auto closeBracket = line.find(']', comma2 + 1);

                rv.vec_values[3] = line.substr(openBracket + 1, comma0 - openBracket - 1);
                rv.vec_values[2] = line.substr(comma0 + 2, comma1 - comma0 - 2);
                rv.vec_values[1] = line.substr(comma1 + 2, comma2 - comma1 - 2);
                rv.vec_values[0] = line.substr(comma2 + 2, closeBracket - comma2 - 2);
              } else {
                rv.value = line.substr(secondSpaceIndex + 1);
                rv.is_float = (line.find('.', secondSpaceIndex) != std::string::npos);
              }
              spec.outputs.push_back(rv);
            } else {
              auto memoryOutIndex = line.find("MEMORY_OUT");
              if (memoryOutIndex != std::string::npos) {
                auto spaceIndex = line.find(' ', memoryOutIndex);
                auto secondSpaceIndex = line.find(' ', spaceIndex + 1);

                TestSpec::MemValue mv;
                mv.address = line.substr(spaceIndex + 1, secondSpaceIndex - spaceIndex - 1);
                mv.data = parse_hex_string(line.substr(secondSpaceIndex + 1));
                spec.mem_outputs.push_back(mv);
              }
            }
          }
        } while (getline() && !line.empty() && line[0] == '#');

        // Only add specs that have actual test directives
        // Skip helper functions with no inputs/outputs
        if (!spec.inputs.empty() || !spec.outputs.empty() || !spec.mem_inputs.empty() ||
            !spec.mem_outputs.empty()) {
          specs.push_back(std::move(spec));
        }
      }
    }
  }

  return specs;
}

/// Extract symbols from disassembly file
std::unordered_map<std::string, std::string> parse_disassembly(
    const std::string& disPath, const std::string& stem,
    const std::unordered_set<size_t>& validAddresses) {
  std::unordered_map<std::string, std::string> symbols;
  std::ifstream in(disPath);
  if (!in.is_open()) {
    return symbols;
  }

  std::string line;
  while (std::getline(in, line)) {
    auto spaceIndex = line.find(' ');
    auto bracketIndex = line.find('>');
    if (spaceIndex != std::string::npos && bracketIndex != std::string::npos) {
      size_t address = ~0ull;
      std::from_chars(&line[0], &line[spaceIndex], address, 16);
      address &= 0xFFFFF;  // Mask to test addresses
      if (validAddresses.find(address) != validAddresses.end()) {
        auto name = line.substr(spaceIndex + 2, bracketIndex - spaceIndex - 2);
        symbols.emplace(name, fmt::format("{}_{:X}", stem, address));
      }
    }
  }
  return symbols;
}

}  // anonymous namespace

bool recompile_tests(const std::string_view& binDirPath, const std::string_view& asmDirPath,
                     const std::string_view& outDirPath) {
  REXLOG_INFO("Recompiling PPC tests...");
  REXLOG_INFO("  Bin dir: {}", binDirPath);
  REXLOG_INFO("  ASM dir: {}", asmDirPath);
  REXLOG_INFO("  Output dir: {}", outDirPath);

  // Create output directory
  fs::create_directories(outDirPath);

  // Track all functions per file
  std::map<std::string, std::unordered_set<size_t>> functionsByFile;
  std::vector<std::string> allFunctionNames;
  std::stringstream functionsOut;

  // Process each .bin file
  for (const auto& entry : fs::directory_iterator(binDirPath)) {
    if (entry.path().extension() != ".bin") {
      continue;
    }

    auto stem = entry.path().stem().string();
    REXLOG_DEBUG("Processing binary file: {}", stem);

    // Load the linked binary file
    std::vector<uint8_t> fileData;
    {
      std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
      if (!file) {
        REXLOG_WARN("Failed to load binary file: {}", entry.path().string());
        continue;
      }
      auto size = file.tellg();
      file.seekg(0, std::ios::beg);
      fileData.resize(static_cast<size_t>(size));
      file.read(reinterpret_cast<char*>(fileData.data()), size);
    }
    if (fileData.empty()) {
      continue;
    }

    // Load map file (required)
    auto mapPath = fmt::format("{}/{}.map", binDirPath, stem);
    auto symbols = parse_map_file(mapPath);
    if (symbols.empty()) {
      REXLOG_ERROR("No symbols found in map file: {}", mapPath);
      continue;
    }

    // Create TestModule and load the binary data
    codegen::TestModule module;
    module.Load(TEST_BASE_ADDRESS, fileData.data(), fileData.size());
    module.set_name(stem);

    // Create CodegenContext from our module
    codegen::RecompilerConfig config;
    config.outDirectoryPath = std::string(outDirPath);
    auto ctx =
        codegen::CodegenContext::Create(codegen::BinaryView::fromModule(module), std::move(config));

    // Analyze functions using test_ prefixed symbols from map
    codegen::AnalyzeTestBinary(ctx, stem, symbols, TEST_BASE_ADDRESS, fileData.data(),
                               fileData.size());

    REXLOG_DEBUG("  Found {} functions", ctx.graph.functionCount());

    // Build sorted function list from graph
    std::vector<const codegen::FunctionNode*> functions;
    for (const auto& [addr, node] : ctx.graph.functions()) {
      functions.push_back(node.get());
    }
    std::sort(functions.begin(), functions.end(),
              [](const auto* a, const auto* b) { return a->base() < b->base(); });

    // Build EmitContext for FunctionNode::emitCpp()
    codegen::EmitContext emitCtx{ctx.binary(), ctx.Config(), ctx.graph, 0, nullptr};

    // Recompile each function
    std::string recompiledCode;
    for (const auto* fn : functions) {
      std::string code = fn->emitCpp(emitCtx);
      if (!code.empty()) {
        functionsByFile[stem].emplace(fn->base());
        allFunctionNames.push_back(fmt::format("{}_{:X}", stem, fn->base()));
        recompiledCode += code;
      }
    }

    // Append to output
    functionsOut << recompiledCode;
    functionsOut << "\n";
  }

  // Write ppc_config.h for tests
  {
    std::ofstream out(fmt::format("{}/ppc_config.h", outDirPath));
    out << "// Generated by rexglue recompile-tests\n";
    out << "#ifndef PPC_CONFIG_H_INCLUDED\n";
    out << "#define PPC_CONFIG_H_INCLUDED\n\n";
    out << "// Test code base address (matches -Ttext=0x82010000 linker option)\n";
    out << "#define PPC_IMAGE_BASE 0x82010000ull\n";
    out << "#define PPC_IMAGE_SIZE 0x100000ull\n";
    out << "#define PPC_CODE_BASE 0x82010000ull\n";
    out << "#define PPC_CODE_SIZE 0x100000ull\n\n";
    out << "#endif // PPC_CONFIG_H_INCLUDED\n";
  }

  // Write functions file with header and forward declarations
  {
    std::ofstream out(fmt::format("{}/ppc_test_functions.cpp", outDirPath));

    // Write header
    out << "// Generated by rexglue recompile-tests\n";
    out << "// DO NOT EDIT - this file is auto-generated\n\n";
    out << "#include \"ppc_config.h\"\n";
    out << "#include <rex/ppc.h>\n";
    out << "#include <rex/logging.h>  // For REX_FATAL on unresolved calls\n";

    // Write forward declarations for all functions
    out << "// Forward declarations\n";
    for (const auto& funcName : allFunctionNames) {
      out << fmt::format("PPC_EXTERN_FUNC({});\n", funcName);
    }
    out << "\n";

    // Write function implementations
    out << functionsOut.str();
  }

  // Build symbol table from map files
  std::unordered_map<std::string, std::string> allSymbols;
  for (const auto& [stem, addresses] : functionsByFile) {
    // Load map file to get symbol name -> address mapping
    auto mapPath = fmt::format("{}/{}.map", binDirPath, stem);
    auto mapSymbols = parse_map_file(mapPath);

    // Convert: for each address we have, find the symbol name from the map
    // Then create: label_name -> stem_ADDR
    for (const auto& [addr, name] : mapSymbols) {
      if (addresses.count(addr)) {
        allSymbols.emplace(name, fmt::format("{}_{:X}", stem, addr));
      }
    }

    // Fallback: if no map file, parse assembly to match labels with addresses
    if (mapSymbols.empty()) {
      std::vector<size_t> sortedAddresses(addresses.begin(), addresses.end());
      std::sort(sortedAddresses.begin(), sortedAddresses.end());

      std::ifstream in(fmt::format("{}/{}.s", asmDirPath, stem));
      if (in.is_open()) {
        std::string line;
        size_t functionIndex = 0;

        while (std::getline(in, line)) {
          if (line.empty() || line[0] == '#' || line[0] == '/' || line[0] == '*') {
            continue;
          }

          auto colonIdx = line.find(':');
          if (colonIdx != std::string::npos && line[0] != ' ' && line[0] != '\t') {
            if (line[0] == '.')
              continue;
            auto name = line.substr(0, colonIdx);

            if (functionIndex < sortedAddresses.size()) {
              allSymbols.emplace(name,
                                 fmt::format("{}_{:X}", stem, sortedAddresses[functionIndex]));
              functionIndex++;
            }
          }
        }
      }
    }
  }

  // Parse test specifications and generate Catch2 test cases
  std::stringstream testsOut;
  testsOut << "// Generated by rexglue recompile-tests\n";
  testsOut << "// DO NOT EDIT - this file is auto-generated\n\n";
  testsOut << "#include <catch2/catch_test_macros.hpp>\n";
  testsOut << "#include <cstdint>\n";
  testsOut << "#include <cstring>\n";
  testsOut << "#include <stdexcept>\n";
  testsOut << "#include \"ppc_config.h\"\n";
  testsOut << "#include <rex/ppc.h>\n";
  testsOut << "#include <rex/system/xmemory.h>\n";
  testsOut << "#include \"ppc_test_decls.h\"\n\n";
  testsOut << "// Global memory instance - same class used by real runtime\n";
  testsOut << "static rex::memory::Memory& get_memory() {\n";
  testsOut << "    static rex::memory::Memory memory;\n";
  testsOut << "    static bool initialized = false;\n";
  testsOut << "    if (!initialized) {\n";
  testsOut << "        if (!memory.Initialize()) {\n";
  testsOut << "            throw std::runtime_error(\"Failed to initialize memory\");\n";
  testsOut << "        }\n";
  testsOut << "        // Allocate test memory region starting after null guard page (0x10000)\n";
  testsOut << "        auto* heap = memory.LookupHeap(0x10000);\n";
  testsOut << "        if (heap) {\n";
  testsOut
      << "            // Allocate from end of guard page (0x10000) up to cover test addresses\n";
  testsOut << "            heap->AllocFixed(0x10000, 0x10000000, 0x1000,\n";
  testsOut << "                rex::memory::kMemoryAllocationReserve | "
              "rex::memory::kMemoryAllocationCommit,\n";
  testsOut
      << "                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);\n";
  testsOut << "        }\n";
  testsOut << "        initialized = true;\n";
  testsOut << "    }\n";
  testsOut << "    return memory;\n";
  testsOut << "}\n\n";

  // Generate declarations file
  std::stringstream declsOut;
  declsOut << "// Generated by rexglue recompile-tests\n";
  declsOut << "#pragma once\n\n";
  declsOut << "#include \"ppc_config.h\"\n";
  declsOut << "#include <rex/ppc.h>\n\n";

  size_t totalTests = 0;

  for (const auto& [stem, addresses] : functionsByFile) {
    auto asmPath = fmt::format("{}/{}.s", asmDirPath, stem);
    auto specs = parse_test_specs(asmPath, allSymbols);

    for (const auto& spec : specs) {
      // Generate declaration
      declsOut << fmt::format("PPC_EXTERN_FUNC({});\n", spec.symbol);

      // Determine category tag from stem
      std::string category = "misc";
      if (stem.find("add") != std::string::npos || stem.find("sub") != std::string::npos ||
          stem.find("mul") != std::string::npos || stem.find("div") != std::string::npos) {
        category = "arithmetic";
      } else if (stem.find("cmp") != std::string::npos) {
        category = "comparison";
      } else if (stem.find("and") != std::string::npos || stem.find("or") != std::string::npos ||
                 stem.find("xor") != std::string::npos || stem.find("rl") != std::string::npos) {
        category = "logical";
      } else if (stem.find("f") == 0 || stem.find("_f") != std::string::npos) {
        category = "floating_point";
      } else if (stem.find("v") == 0 || stem.find("_v") != std::string::npos) {
        category = "vector";
      } else if (stem.find("l") == 0 || stem.find("st") == 0) {
        category = "memory";
      }

      // Generate Catch2 TEST_CASE
      testsOut << fmt::format("TEST_CASE(\"{}\", \"[ppc][{}][{}]\") {{\n", spec.name, category,
                              stem);
      testsOut << "    auto& mem = get_memory();\n";
      testsOut << "    uint8_t* memory = mem.virtual_membase();\n";
      testsOut << "    PPCContext ctx{};\n";
      testsOut << "    ctx.fpscr.loadFromHost();\n\n";

      // REGISTER_IN
      for (const auto& rv : spec.inputs) {
        if (rv.reg == "cr") {
          testsOut << fmt::format("    ctx.cr0.set_raw(({} >> 28) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr1.set_raw(({} >> 24) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr2.set_raw(({} >> 20) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr3.set_raw(({} >> 16) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr4.set_raw(({} >> 12) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr5.set_raw(({} >> 8) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr6.set_raw(({} >> 4) & 0xF);\n", rv.value);
          testsOut << fmt::format("    ctx.cr7.set_raw({} & 0xF);\n", rv.value);
        } else if (rv.is_vector) {
          testsOut << fmt::format("    ctx.{}.u32[3] = 0x{};\n", rv.reg, rv.vec_values[3]);
          testsOut << fmt::format("    ctx.{}.u32[2] = 0x{};\n", rv.reg, rv.vec_values[2]);
          testsOut << fmt::format("    ctx.{}.u32[1] = 0x{};\n", rv.reg, rv.vec_values[1]);
          testsOut << fmt::format("    ctx.{}.u32[0] = 0x{};\n", rv.reg, rv.vec_values[0]);
        } else if (rv.is_float) {
          testsOut << fmt::format("    ctx.{}.f64 = {};\n", rv.reg, rv.value);
        } else {
          testsOut << fmt::format("    ctx.{}.u64 = {};\n", rv.reg, rv.value);
        }
      }

      // MEMORY_IN
      for (const auto& mv : spec.mem_inputs) {
        for (size_t i = 0; i < mv.data.size(); ++i) {
          testsOut << fmt::format("    memory[0x{} + 0x{:X}] = 0x{:02X};\n", mv.address, i,
                                  mv.data[i]);
        }
      }

      testsOut << "\n";
      testsOut << fmt::format("    {}(ctx, memory);\n\n", spec.symbol);

      // REGISTER_OUT - generate REQUIRE assertions
      for (const auto& rv : spec.outputs) {
        if (rv.reg == "cr") {
          testsOut << "    {\n";
          testsOut << "        uint32_t cr_actual = "
                      "(ctx.cr0.raw() << 28) | (ctx.cr1.raw() << 24) | "
                      "(ctx.cr2.raw() << 20) | (ctx.cr3.raw() << 16) | "
                      "(ctx.cr4.raw() << 12) | (ctx.cr5.raw() << 8) | "
                      "(ctx.cr6.raw() << 4) | ctx.cr7.raw();\n";
          testsOut << fmt::format("        REQUIRE(cr_actual == {});\n", rv.value);
          testsOut << "    }\n";
        } else if (rv.is_vector) {
          testsOut << fmt::format("    REQUIRE(ctx.{}.u32[3] == 0x{});\n", rv.reg,
                                  rv.vec_values[3]);
          testsOut << fmt::format("    REQUIRE(ctx.{}.u32[2] == 0x{});\n", rv.reg,
                                  rv.vec_values[2]);
          testsOut << fmt::format("    REQUIRE(ctx.{}.u32[1] == 0x{});\n", rv.reg,
                                  rv.vec_values[1]);
          testsOut << fmt::format("    REQUIRE(ctx.{}.u32[0] == 0x{});\n", rv.reg,
                                  rv.vec_values[0]);
        } else if (rv.is_float) {
          testsOut << fmt::format("    REQUIRE(ctx.{}.f64 == {});\n", rv.reg, rv.value);
        } else {
          testsOut << fmt::format("    REQUIRE(ctx.{}.u64 == {});\n", rv.reg, rv.value);
        }
      }

      // MEMORY_OUT
      for (const auto& mv : spec.mem_outputs) {
        for (size_t i = 0; i < mv.data.size(); ++i) {
          testsOut << fmt::format("    REQUIRE(memory[0x{} + 0x{:X}] == 0x{:02X});\n", mv.address,
                                  i, mv.data[i]);
        }
      }

      testsOut << "}\n\n";
      totalTests++;
    }
  }

  // Write test cases file
  {
    std::ofstream out(fmt::format("{}/ppc_test_cases.cpp", outDirPath));
    out << testsOut.str();
  }

  // Write declarations file
  {
    std::ofstream out(fmt::format("{}/ppc_test_decls.h", outDirPath));
    out << declsOut.str();
  }

  REXLOG_INFO("Generated {} test cases", totalTests);
  return true;
}

}  // namespace rexglue::commands
