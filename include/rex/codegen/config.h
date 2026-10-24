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

  bool ret = false;             /**< Return from the recompiled function after the call */
  bool return_on_true = false;  /**< Return when the function returns true */
  bool return_on_false = false; /**< Return when the function returns false */

  u32 jump_address = 0;          /**< Address to jump to after the call, 0 for none */
  u32 jump_address_on_true = 0;  /**< Address to jump to when the function returns true */
  u32 jump_address_on_false = 0; /**< Address to jump to when the function returns false */

  bool after_instruction =
      false; /**< Runs after the instruction at the address instead of before */

  /** Parameter types of a body declared in C++, in order. Empty for a hook from the TOML. */
  std::vector<std::string> parameters;
  bool from_source = false; /**< True for a body declared in C++ and found by the source scan */
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
  bool share_registers = false;

  /**
   * Returns the size, preferring size over end.
   *
   * @param address Function start address
   * @return Size in bytes, 0 when neither field gives one
   */
  u32 GetSize(u32 address) const { return size ? size : (end > address ? end - address : 0); }

  /**
   * Reports whether this is a discontinuous chunk of a parent function.
   *
   * @return True when parent is non zero
   */
  bool IsChunk() const { return parent != 0; }
};

/** Section info for analysis output. */
struct Section {
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
 * Settings that control how a binary is analyzed and emitted, loaded from TOML.
 */
struct RecompilerConfig {
  RecompilerConfig() = default;
  RecompilerConfig(const RecompilerConfig&) = default;
  RecompilerConfig& operator=(const RecompilerConfig&) = default;
  RecompilerConfig(RecompilerConfig&&) noexcept(false) = default;
  RecompilerConfig& operator=(RecompilerConfig&&) noexcept(false) = default;
  ~RecompilerConfig() = default;

  /** TOML key `project_name`, default `rex`. Prefix of the generated `<name>_*.h` and `.cpp` files
   * and the `project` template variable. */
  std::string project_name = "rex";
  std::string file_path; /**< Path to XEX or ELF file */
  /** TOML key `out_directory_path`, default empty. Directory the generated files are written to,
   * relative to the config file directory. */
  std::string out_directory_path;
  std::string template_dir; /**< Optional template directory overriding the built in templates */

  /**
   * Every TOML that fed this config, in load order. LoadFromTable() gets a parsed table, so a
   * manifest embedded entry omits the manifest itself.
   */
  std::vector<std::string> loaded_files;

  /**
   * Warnings about keys the loader does not read or reads with the wrong type, in load order. Each
   * names the file and the key path. They never fail the load.
   */
  std::vector<std::string> load_warnings;

  /** TOML key `skip_lr`, default false. When true, omits the `ctx.lr` store at calls between title
   * functions and the `mflr` read. Calls to imports always store `ctx.lr`. */
  bool skip_lr = false;
  /** TOML key `ctr_as_local`, default false. When true, keeps CTR in a local variable instead of
   * `ctx.ctr`. */
  bool ctr_as_local_variable = false;
  /** TOML key `xer_as_local`, default false. When true, keeps XER in a local variable instead of
   * `ctx.xer`. */
  bool xer_as_local_variable = false;
  /** TOML key `reserved_as_local`, default false. When true, keeps the reservation register in a
   * local variable instead of `ctx.reserved`. */
  bool reserved_register_as_local_variable = false;
  /** TOML key `skip_msr`, default false. When true, omits the generated global lock check that
   * mfmsr emits. */
  bool skip_msr = false;
  /** TOML key `cr_as_local`, default false. When true, keeps the CR fields in local variables
   * instead of `ctx.cr0` to `ctx.cr7`. */
  bool cr_registers_as_local_variables = false;
  /** TOML key `non_argument_as_local`, default false. When true, keeps r0, r11, r12, f0 and v32 to
   * v63 in local variables. r2 and r13 always stay in ctx. */
  bool non_argument_registers_as_local_variables = false;
  /** TOML key `non_volatile_as_local`, default false. When true, keeps r14 and above, f14 and above
   * and the high vector registers in local variables, except in register sharing functions and SEH
   * scopes. */
  bool non_volatile_registers_as_local_variables = false;
  bool generate_exception_handlers = false; /**< Generates SEH exception handler wrappers */

  u32 data_region_threshold = 16; /**< Invalid instructions in a row that mark a data region */

  /**
   * Overrides the DLL module flag. When unset the orchestrator infers it from the module
   * position in the manifest, with the entrypoint false and modules true.
   */
  std::optional<bool> is_dll;

  /** TOML tables `[functions]` keyed by hex entry address. Overrides the size, end, name and parent
   * of discovered functions. */
  std::unordered_map<u32, FunctionConfig> functions;
  /** TOML array `[[switch_tables]]` keyed by the bctr address. Manual jump tables that replace
   * automatic detection. */
  std::unordered_map<u32, JumpTable> switch_tables;
  /** TOML array `[[midasm_hook]]` keyed by instruction address. Calls to user functions inserted
   * into the generated code. */
  std::unordered_map<u32, MidAsmHook> mid_asm_hooks;
  /** TOML key `longjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_longjmp`. */
  u32 longjmp_address = 0;
  /** TOML key `setjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_setjmp`. */
  u32 setjmp_address = 0;

  /** Shim function addresses keyed by registry name, read from the [shim] toml table. */
  std::unordered_map<std::string, u32> shim_functions;

  std::unordered_map<u32, u32> invalid_instruction_hints; /**< Maps address to size */
  /** Loads and stores the [mmio.gpu] and [mmio.apu] tables route to a register block. */
  std::unordered_map<u32, MMIODeclaredAccess> declared_mmio_accesses;
  /** TOML array `analysis.exception_handler_funcs`. Entry addresses analysis treats as exception
   * handler functions, sorted and deduplicated after load. */
  std::vector<u32> exception_handler_func_hints;

  /**
   * Loads configuration from a TOML file.
   *
   * Supports an optional `includes` array for layered config. Paths in `includes` resolve
   * relative to the including file directory. Scalars take the last value, keyed tables are
   * additive with the last value winning on a repeated key, arrays of tables are deduplicated
   * by primary key and sets are additive.
   *
   * @param config_file_path Path to the TOML config file
   * @return True on success, false on parse failure, circular include or depth exceeded
   */
  bool Load(const std::string_view& config_file_path);

  /**
   * Loads configuration from an in memory TOML table such as an inline binary entry inside a
   * manifest. Includes referenced from the table resolve relative to `base_dir`. Merges as
   * Load() does.
   *
   * @param tbl Parsed TOML table
   * @param base_dir Directory that includes resolve against
   * @param source The file and section the table came from, the start of key warnings
   * @return True on success, false on error
   */
  bool LoadFromTable(const toml::table& tbl, const std::filesystem::path& base_dir,
                     const std::string& source = "<inline>");

  /**
   * Loads a table that several binaries share, such as the top level keys of a manifest. Applies
   * the table and its includes as LoadFromTable() does, but skips the finalize checks because the
   * shared layer is not a complete binary config.
   *
   * @param tbl Parsed TOML table
   * @param base_dir Directory that includes resolve against
   * @param source The file and section the table came from, the start of key warnings
   * @return True on success, false on error
   */
  bool LoadShared(const toml::table& tbl, const std::filesystem::path& base_dir,
                  const std::string& source = "<shared>");

  /**
   * Removes every entry that belongs to the binary containing its address, so a copy of a shared
   * layer keeps only the defaults that apply to all binaries.
   */
  void ClearAddressEntries();

  /**
   * Adds the entries of a shared layer whose address is inside [base, base + size). An entry
   * this config has keeps its own value.
   *
   * @param shared The shared layer.
   * @param base First address of the binary image.
   * @param size Image size in bytes.
   */
  void MergeSharedEntriesIn(const RecompilerConfig& shared, u32 base, u32 size);

  /**
   * Lists the entries of this shared layer whose address is in none of the image ranges.
   *
   * @param ranges Pairs of image base address and image size.
   * @return One text per entry naming its key and address, empty when every entry is inside an
   * image.
   */
  std::vector<std::string> ListEntriesOutside(const std::vector<std::pair<u32, u32>>& ranges) const;

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
