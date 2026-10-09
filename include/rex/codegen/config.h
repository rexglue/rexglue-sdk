/**
 * @file        codegen/config.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/mmio.h>

namespace rex::codegen {

/**
 * User function the generated code calls at an instruction address, with an optional return or
 * jump.
 */
struct MidAsmHook {
  std::string name;                   /**< Name of the function to call */
  std::vector<std::string> registers; /**< Registers passed as arguments in order */

  bool ret = false;           /**< Return from the recompiled function after the call */
  bool returnOnTrue = false;  /**< Return when the function returns true */
  bool returnOnFalse = false; /**< Return when the function returns false */

  u32 jumpAddress = 0;        /**< Address to jump to after the call, 0 for none */
  u32 jumpAddressOnTrue = 0;  /**< Address to jump to when the function returns true */
  u32 jumpAddressOnFalse = 0; /**< Address to jump to when the function returns false */

  bool afterInstruction = false; /**< Runs after the instruction at the address instead of before */
};

/** A load or store the [mmio.gpu] or [mmio.apu] table routes to a register block. */
struct MMIODeclaredAccess {
  /** The register block the instruction reaches. */
  MMIOBlock block;
  /** True when the table lists the instruction under write. */
  bool write;
};

/**
 * Function or chunk configuration. A chunk is a function entry with a non zero parent.
 */
struct FunctionConfig {
  u32 size = 0;     /**< Size in bytes, exclusive with end */
  u32 end = 0;      /**< Exclusive end address, exclusive with size */
  std::string name; /**< Symbol name, empty generates sub_XXXXXXXX */
  u32 parent = 0;   /**< Parent function address, 0 when standalone */

  /**
   * Keeps non volatiles in ctx instead of localizing them. Marks an MSVC SEH funclet, which
   * reads the registers its owner left live and would see a zero from a local. Callers sync
   * their localized copies across the call site.
   */
  bool shareRegisters = false;

  /**
   * Returns the size, preferring size over end.
   *
   * @param address Function start address
   * @return Size in bytes, 0 when neither field gives one
   */
  u32 getSize(u32 address) const { return size ? size : (end > address ? end - address : 0); }

  /**
   * Reports whether this is a discontinuous chunk of a parent function.
   *
   * @return True when parent is non zero
   */
  bool isChunk() const { return parent != 0; }
};

/** Section info for analysis output. */
struct SectionInfo {
  std::string name;  /**< Section name written to the analysis output. */
  u64 address = 0;   /**< Guest address of the first byte of the section. */
  u64 size = 0;      /**< Section size in bytes. */
  std::string flags; /**< "rx", "rw" or "r" */
};

/** Function entry for analysis output. */
struct FunctionEntry {
  u64 address = 0;  /**< Guest address of the function entry. */
  u64 size = 0;     /**< Function size in bytes. */
  std::string name; /**< Empty defaults to sub_XXXXXXXX */
};

/**
 * Settings that control how 1 binary is analyzed and emitted, loaded from TOML.
 */
struct RecompilerConfig {
  /** TOML key `project_name`, default `rex`. Prefix of the generated `<name>_*.h` and `.cpp` files
   * and the `project` template variable. */
  std::string projectName = "rex";
  std::string filePath; /**< Path to XEX or ELF file */
  /** TOML key `out_directory_path`, default empty. Directory the generated files are written to,
   * relative to the config file directory. */
  std::string outDirectoryPath;
  std::string templateDir; /**< Optional template directory overriding the built in templates */

  /**
   * Every TOML that fed this config, in load order. LoadFromTable() gets a parsed table, so a
   * manifest embedded entry omits the manifest itself.
   */
  std::vector<std::string> loadedFiles;

  /** TOML key `skip_lr`, default false. When true, omits the generated writes and reads of `ctx.lr`
   * at bl, mflr and similar instructions. */
  bool skipLr = false;
  /** TOML key `ctr_as_local`, default false. When true, keeps CTR in a local variable instead of
   * `ctx.ctr`. */
  bool ctrAsLocalVariable = false;
  /** TOML key `xer_as_local`, default false. When true, keeps XER in a local variable instead of
   * `ctx.xer`. */
  bool xerAsLocalVariable = false;
  /** TOML key `reserved_as_local`, default false. When true, keeps the reservation register in a
   * local variable instead of `ctx.reserved`. */
  bool reservedRegisterAsLocalVariable = false;
  /** TOML key `skip_msr`, default false. When true, omits the generated global lock check that
   * mfmsr emits. */
  bool skipMsr = false;
  /** TOML key `cr_as_local`, default false. When true, keeps the CR fields in local variables
   * instead of `ctx.cr0` to `ctx.cr7`. */
  bool crRegistersAsLocalVariables = false;
  /** TOML key `non_argument_as_local`, default false. When true, keeps r0, r2, r11, r12, f0 and v32
   * to v63 in local variables. */
  bool nonArgumentRegistersAsLocalVariables = false;
  /** TOML key `non_volatile_as_local`, default false. When true, keeps r14 and above, f14 and above
   * and the high vector registers in local variables, except in register sharing functions and SEH
   * scopes. */
  bool nonVolatileRegistersAsLocalVariables = false;
  bool generateExceptionHandlers = false; /**< Generates SEH exception handler wrappers */

  u32 maxJumpExtension = 65536; /**< Max bytes to extend a function for jump table targets */
  u32 dataRegionThreshold = 16; /**< Invalid instructions in a row that mark a data region */
  u32 largeFunctionThreshold = 1048576; /**< Size in bytes that triggers a warning */

  /**
   * Overrides the DLL module flag. When unset the orchestrator infers it from the module
   * position in the manifest, with the entrypoint false and modules true.
   */
  std::optional<bool> isDll;

  /** TOML tables `[functions]` keyed by hex entry address. Overrides the size, end, name and parent
   * of discovered functions. */
  std::unordered_map<u32, FunctionConfig> functions;
  /** TOML array `[[switch_tables]]` keyed by the bctr address. Manual jump tables that replace
   * automatic detection. */
  std::unordered_map<u32, JumpTable> switchTables;
  /** TOML array `[[midasm_hook]]` keyed by instruction address. Calls to user functions inserted
   * into the generated code. */
  std::unordered_map<u32, MidAsmHook> midAsmHooks;
  /** TOML key `longjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_longjmp`. */
  u32 longJmpAddress = 0;
  /** TOML key `setjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_setjmp`. */
  u32 setJmpAddress = 0;

  /** Shim function addresses keyed by registry name, read from the [shim] toml table. */
  std::unordered_map<std::string, u32> shimFunctions;

  std::unordered_map<u32, u32> invalidInstructionHints; /**< Maps address to size */
  /** Addresses of bctr instructions that are vtable or computed calls. */
  std::unordered_set<u32> knownIndirectCallHints;
  /** Loads and stores the [mmio.gpu] and [mmio.apu] tables route to a register block. */
  std::unordered_map<u32, MMIODeclaredAccess> declaredMMIOAccesses;
  /** TOML array `analysis.exception_handler_funcs`. Entry addresses analysis treats as exception
   * handler functions, sorted and deduplicated after load. */
  std::vector<u32> exceptionHandlerFuncHints;

  /**
   * Loads configuration from a TOML file.
   *
   * Supports an optional `includes` array for layered config. Paths in `includes` resolve
   * relative to the including file directory. Scalars take the last value, keyed tables are
   * additive with the last value winning on a repeated key, arrays of tables are deduplicated
   * by primary key and sets are additive.
   *
   * @param configFilePath Path to the TOML config file
   * @return True on success, false on parse failure, circular include or depth exceeded
   */
  bool Load(const std::string_view& configFilePath);

  /**
   * Loads configuration from an in memory TOML table such as an inline binary entry inside a
   * manifest. Includes referenced from the table resolve relative to `base_dir`. Merges as
   * Load() does.
   *
   * @param tbl Parsed TOML table
   * @param base_dir Directory that includes resolve against
   * @return True on success, false on error
   */
  bool LoadFromTable(const toml::table& tbl, const std::filesystem::path& base_dir);

  /**
   * Reports whether the configured shim functions include the heap group.
   *
   * @return True when any configured shim function requires the shim heap.
   */
  bool UsesShimHeap() const;

  /** Validation result containing warnings and errors. */
  struct ValidationResult {
    bool valid = true; /**< True when there are no errors, warnings allowed */
    /** Non fatal issues such as an empty `file_path` or `out_directory_path`. */
    std::vector<std::string> warnings;
    std::vector<std::string> errors; /**< Fatal issues that block codegen */

    explicit operator bool() const { return valid; }
  };

  /**
   * Validates the loaded configuration.
   * Checks address alignment, required fields and sanity constraints.
   *
   * @return ValidationResult with warnings and errors
   */
  ValidationResult Validate() const;
};

}  // namespace rex::codegen
