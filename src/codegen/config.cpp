/**
 * @file        rexcodegen/config.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp toml config
 */

// TOML config file loading

#include <algorithm>
#include <map>

#include <fmt/format.h>

#include <rex/codegen/config.h>
#include <rex/logging.h>

#include "codegen_logging.h"

#include <toml++/toml.hpp>

namespace rex::codegen {

void RecompilerConfig::Load(const std::string_view& configFilePath) {
  toml::table toml;

  try {
    toml = toml::parse_file(configFilePath);
  } catch (const toml::parse_error& e) {
    REXCODEGEN_ERROR("Failed to parse config file '{}': {}", configFilePath, e.what());
    return;
  }

  // Required fields (flat format)
  projectName = toml["project_name"].value_or<std::string>("rex");
  filePath = toml["file_path"].value_or<std::string>("");
  outDirectoryPath = toml["out_directory_path"].value_or<std::string>("generated");

  if (filePath.empty()) {
    REXCODEGEN_ERROR("Missing required field: file_path");
  }

  // Optional patch fields
  patchFilePath = toml["patch_file_path"].value_or<std::string>("");
  patchedFilePath = toml["patched_file_path"].value_or<std::string>("");

  // Optimization options
  skipLr = toml["skip_lr"].value_or(false);
  skipMsr = toml["skip_msr"].value_or(false);
  ctrAsLocalVariable = toml["ctr_as_local"].value_or(false);
  xerAsLocalVariable = toml["xer_as_local"].value_or(false);
  reservedRegisterAsLocalVariable = toml["reserved_as_local"].value_or(false);
  crRegistersAsLocalVariables = toml["cr_as_local"].value_or(false);
  nonArgumentRegistersAsLocalVariables = toml["non_argument_as_local"].value_or(false);
  nonVolatileRegistersAsLocalVariables = toml["non_volatile_as_local"].value_or(false);

  // Special addresses (user overrides)
  longJmpAddress = toml["longjmp_address"].value_or(0u);
  setJmpAddress = toml["setjmp_address"].value_or(0u);

  // rexcrt function addresses
  if (auto* rexcrt = toml["rexcrt"].as_table()) {
    for (const auto& [name, val] : *rexcrt) {
      if (auto addr = val.value<int64_t>()) {
        rexcrtFunctions[std::string(name)] = static_cast<uint32_t>(*addr);
      }
    }
  }

  // Helper to parse hex address from string key
  auto parseHexAddress = [](const std::string& keyStr) -> std::optional<uint32_t> {
    try {
      if (keyStr.starts_with("0x") || keyStr.starts_with("0X")) {
        return static_cast<uint32_t>(std::stoul(keyStr.substr(2), nullptr, 16));
      } else {
        return static_cast<uint32_t>(std::stoul(keyStr, nullptr, 16));
      }
    } catch (...) {
      return std::nullopt;
    }
  };

  // Format: address = { size = N } or { end = N } or { parent = P, end = N } etc.
  if (auto functionsTable = toml["functions"].as_table()) {
    for (auto& [key, value] : *functionsTable) {
      std::string keyStr(key.str());
      auto addrOpt = parseHexAddress(keyStr);
      if (!addrOpt) {
        REXCODEGEN_ERROR("Invalid function address key: {}", keyStr);
        continue;
      }
      uint32_t address = *addrOpt;

      auto* table = value.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [functions] entry at 0x{:08X}: expected table", address);
        continue;
      }

      FunctionConfig cfg;
      cfg.size = (*table)["size"].value_or(0u);
      cfg.end = (*table)["end"].value_or(0u);
      cfg.name = (*table)["name"].value_or(std::string{});
      cfg.parent = (*table)["parent"].value_or(0u);

      // Validation: can't have both size and end
      if (cfg.size && cfg.end) {
        REXCODEGEN_ERROR("Function 0x{:08X}: cannot specify both 'size' and 'end'", address);
        continue;
      }

      // Validation: end must be > address
      if (cfg.end && cfg.end <= address) {
        REXCODEGEN_ERROR("Function 0x{:08X}: 'end' (0x{:08X}) must be greater than address",
                         address, cfg.end);
        continue;
      }

      functions.emplace(address, std::move(cfg));
    }

    if (!functions.empty()) {
      size_t chunks_count = 0;
      for (const auto& [addr, cfg] : functions) {
        if (cfg.isChunk())
          chunks_count++;
      }
      REXCODEGEN_INFO("Loaded {} function configs ({} standalone, {} chunks)", functions.size(),
                      functions.size() - chunks_count, chunks_count);
    }
  }

  // Invalid instruction hints
  // Data patterns that look like code but aren't (e.g., embedded constants)
  if (auto invalidArray = toml["invalid_instructions"].as_array()) {
    for (auto& entry : *invalidArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[invalid_instructions]] entry: expected table");
        continue;
      }

      auto data_opt = (*table)["data"].value<uint32_t>();
      auto size_opt = (*table)["size"].value<uint32_t>();

      if (!data_opt) {
        REXCODEGEN_ERROR("Missing 'data' in [[invalid_instructions]] entry");
        continue;
      }
      if (!size_opt) {
        REXCODEGEN_ERROR("Missing 'size' in [[invalid_instructions]] entry");
        continue;
      }

      invalidInstructionHints.emplace(*data_opt, *size_opt);
    }
  }

  // Known indirect call hints (vtable dispatch, computed jumps - not switch tables)
  // These are bctr instructions where the target is loaded from runtime-computed addresses
  if (auto indirectCallArray = toml["indirect_calls"].as_array()) {
    for (auto& entry : *indirectCallArray) {
      if (auto addr = entry.value<int64_t>()) {
        knownIndirectCallHints.insert(static_cast<uint32_t>(*addr));
        REXCODEGEN_DEBUG("Loaded known indirect call hint at 0x{:08X}", *addr);
      }
    }
  }

  // Manual switch table definitions (when auto-detection fails)
  if (auto switchTableArray = toml["switch_tables"].as_array()) {
    for (auto& entry : *switchTableArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[switch_tables]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<uint32_t>();
      auto register_opt = (*table)["register"].value<uint32_t>();
      auto labels_array = (*table)["labels"].as_array();

      if (!address_opt) {
        REXCODEGEN_ERROR("Missing 'address' in [[switch_tables]] entry");
        continue;
      }
      if (!register_opt) {
        REXCODEGEN_ERROR("Missing 'register' in [[switch_tables]] entry");
        continue;
      }
      if (!labels_array) {
        REXCODEGEN_ERROR("Missing 'labels' in [[switch_tables]] entry");
        continue;
      }

      JumpTable jt;
      jt.bctrAddress = *address_opt;
      jt.tableAddress = 0;  // Not specified in config
      jt.indexRegister = static_cast<uint8_t>(*register_opt);

      for (auto& label : *labels_array) {
        // TOML integers are int64_t, cast to uint32_t for addresses
        if (auto label_val = label.value<int64_t>()) {
          jt.targets.push_back(static_cast<uint32_t>(*label_val));
        }
      }

      if (jt.targets.empty()) {
        REXCODEGEN_ERROR("Empty 'labels' array in [[switch_tables]] at 0x{:08X}", *address_opt);
        continue;
      }

      size_t label_count = jt.targets.size();
      switchTables.emplace(*address_opt, std::move(jt));
      REXCODEGEN_DEBUG("Loaded manual switch table at 0x{:08X} with {} labels", *address_opt,
                       label_count);
    }
  }

  // Mid-asm hooks
  if (auto midAsmHookArray = toml["midasm_hook"].as_array()) {
    for (auto& entry : *midAsmHookArray) {
      auto* table = entry.as_table();
      if (!table) {
        REXCODEGEN_ERROR("Invalid [[midasm_hook]] entry: expected table");
        continue;
      }

      auto address_opt = (*table)["address"].value<uint32_t>();
      auto name_opt = (*table)["name"].value<std::string>();

      if (!address_opt) {
        REXCODEGEN_ERROR("Missing 'address' in [[midasm_hook]] entry");
        continue;
      }
      if (!name_opt) {
        REXCODEGEN_ERROR("Missing 'name' in [[midasm_hook]] entry");
        continue;
      }

      MidAsmHook midAsmHook;
      midAsmHook.name = *name_opt;

      if (auto registerArray = (*table)["registers"].as_array()) {
        for (auto& reg : *registerArray) {
          if (auto reg_str = reg.value<std::string>()) {
            midAsmHook.registers.push_back(*reg_str);
          }
        }
      }

      midAsmHook.ret = (*table)["return"].value_or(false);
      midAsmHook.returnOnTrue = (*table)["return_on_true"].value_or(false);
      midAsmHook.returnOnFalse = (*table)["return_on_false"].value_or(false);

      midAsmHook.jumpAddress = (*table)["jump_address"].value_or(0u);
      midAsmHook.jumpAddressOnTrue = (*table)["jump_address_on_true"].value_or(0u);
      midAsmHook.jumpAddressOnFalse = (*table)["jump_address_on_false"].value_or(0u);

      if ((midAsmHook.ret && midAsmHook.jumpAddress != 0) ||
          (midAsmHook.returnOnTrue && midAsmHook.jumpAddressOnTrue != 0) ||
          (midAsmHook.returnOnFalse && midAsmHook.jumpAddressOnFalse != 0)) {
        REXCODEGEN_ERROR("{}: can't return and jump at the same time", midAsmHook.name);
      }

      if ((midAsmHook.ret || midAsmHook.jumpAddress != 0) &&
          (midAsmHook.returnOnFalse || midAsmHook.returnOnTrue ||
           midAsmHook.jumpAddressOnFalse != 0 || midAsmHook.jumpAddressOnTrue != 0)) {
        REXCODEGEN_ERROR("{}: can't mix direct and conditional return/jump", midAsmHook.name);
      }

      midAsmHook.afterInstruction = (*table)["after_instruction"].value_or(false);

      midAsmHooks.emplace(*address_opt, std::move(midAsmHook));
    }
  }

  // Analysis pipeline settings: [analysis] section
  if (auto analysisTable = toml["analysis"].as_table()) {
    maxJumpExtension = (*analysisTable)["max_jump_extension"].value_or(65536u);
    dataRegionThreshold = (*analysisTable)["data_region_threshold"].value_or(16u);
    largeFunctionThreshold = (*analysisTable)["large_function_threshold"].value_or(1048576u);

    // Exception handler function addresses for code region segmentation
    if (auto handlers = (*analysisTable)["exception_handler_funcs"].as_array()) {
      for (const auto& elem : *handlers) {
        if (auto val = elem.value<int64_t>()) {
          exceptionHandlerFuncHints.push_back(static_cast<uint32_t>(*val));
        }
      }
    }
  }
}

RecompilerConfig::ValidationResult RecompilerConfig::Validate() const {
  ValidationResult result;

  // Helper to check 4-byte alignment (PPC instructions are 4-byte aligned)
  auto checkAlignment = [&](uint32_t addr, const char* name) {
    if (addr != 0 && (addr & 0x3) != 0) {
      result.errors.push_back(fmt::format("{} address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  };

  // Check special address alignment
  checkAlignment(longJmpAddress, "longjmp");
  checkAlignment(setJmpAddress, "setjmp");

  for (const auto& [name, addr] : rexcrtFunctions) {
    if (addr & 0x3) {
      result.errors.push_back(
          fmt::format("rexcrt function '{}' address 0x{:08X} is not 4-byte aligned", name, addr));
      result.valid = false;
    }
  }

  // Check function address alignment
  for (const auto& [addr, size] : functions) {
    if (addr & 0x3) {
      result.errors.push_back(fmt::format("Function address 0x{:08X} is not 4-byte aligned", addr));
      result.valid = false;
    }
  }

  // Check for duplicate function boundaries
  {
    std::map<uint32_t, uint32_t> seen;
    for (const auto& [addr, cfg] : functions) {
      uint32_t funcSize = cfg.getSize(addr);
      auto it = seen.find(addr);
      if (it != seen.end()) {
        if (it->second == funcSize) {
          result.errors.push_back(
              fmt::format("Duplicate function boundary: 0x{:08X} size 0x{:X}", addr, funcSize));
        } else {
          result.errors.push_back(fmt::format("Conflicting sizes at 0x{:08X}: 0x{:X} vs 0x{:X}",
                                              addr, it->second, funcSize));
        }
        result.valid = false;
      }
      seen[addr] = funcSize;
    }
  }

  // Check for overlapping function boundaries (standalone functions only)
  {
    std::vector<std::pair<uint32_t, uint32_t>> sorted;
    for (const auto& [addr, cfg] : functions) {
      if (!cfg.isChunk()) {
        sorted.emplace_back(addr, cfg.getSize(addr));
      }
    }
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
      uint32_t prev_end = sorted[i - 1].first + sorted[i - 1].second;
      if (sorted[i].first < prev_end) {
        result.errors.push_back(fmt::format(
            "Overlapping boundaries: 0x{:08X}+0x{:X} overlaps 0x{:08X}+0x{:X}", sorted[i - 1].first,
            sorted[i - 1].second, sorted[i].first, sorted[i].second));
        result.valid = false;
      }
    }
  }

  // Validate rexcrt all-or-nothing groups -- originals are stripped so partial
  // sets would leave the game with missing CRT functions at runtime.
  if (!rexcrtFunctions.empty()) {
    auto checkGroup = [&](const char* groupName, std::initializer_list<const char*> required) {
      size_t found = 0;
      for (const char* name : required) {
        if (rexcrtFunctions.contains(name))
          ++found;
      }
      if (found > 0 && found < required.size()) {
        std::string missing;
        for (const char* name : required) {
          if (!rexcrtFunctions.contains(name)) {
            if (!missing.empty())
              missing += ", ";
            missing += name;
          }
        }
        result.errors.push_back(
            fmt::format("[rexcrt] {} group is incomplete ({}/{} specified), missing: {}", groupName,
                        found, required.size(), missing));
        result.valid = false;
      }
    };

    checkGroup("heap", {"RtlAllocateHeap", "RtlFreeHeap", "RtlSizeHeap", "RtlReAllocateHeap"});
    checkGroup("file", {"CreateFileA", "CloseHandle", "ReadFile", "WriteFile", "SetFilePointer",
                        "GetFileSize", "GetFileAttributesA", "GetFileAttributesExA",
                        "FindFirstFileA", "FindNextFileA", "FindClose", "CreateDirectoryA",
                        "RemoveDirectoryA", "DeleteFileA", "GetCurrentDirectoryA"});
  }

  // Check required fields
  if (filePath.empty()) {
    result.warnings.push_back("file_path is empty");
  }
  if (outDirectoryPath.empty()) {
    result.warnings.push_back("out_directory_path is empty");
  }

  return result;
}

}  // namespace rex::codegen
