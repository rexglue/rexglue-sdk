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

#include "function_graph.h"
#include <rex/ppc/mmio.h>

namespace toml {
inline namespace v3 {
class table;
}  // namespace v3
}  // namespace toml

namespace rex::codegen {

/**
 * User function the generated code calls at an instruction address, with an optional return or
 * jump.
 */
struct MidAsmHook {
  std::string name;
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
  rex::ppc::MMIOBlock block;
  /** True when the table lists the instruction under write. */
  bool write;
};

/** Function or chunk configuration. A chunk is a function entry with a non zero parent. */
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
   * Returns the size, preferring size over end. The size is in bytes, 0 when neither field gives
   * one.
   */
  u32 GetSize(u32 address) const { return size ? size : (end > address ? end - address : 0); }

  /** Reports whether this is a discontinuous chunk of a parent function. */
  bool IsChunk() const { return parent != 0; }
};

struct Section {
  std::string name; /**< Section name written to the analysis output. */
  u64 address = 0;  /**< Guest address of the first byte of the section. */
  u64 size = 0;
  std::string flags; /**< "rx", "rw" or "r" */
};

/** How a vector float instruction is emitted. */
enum class VectorMath : u8 {
  kFast,  /**< Host SSE arithmetic, rounding and denormal results differ from the guest. */
  kExact, /**< Bit exact guest results. */
};

/**
 * TOML table `[vector_math]`, each key `"fast"` or `"exact"`, default fast. A key covers the
 * 128 forms of its instruction. vlogefp is always exact. The fres and frsqrte keys cover the
 * scalar estimates.
 */
struct VectorMathConfig {
  VectorMath vmaddfp = VectorMath::kFast; /**< vmaddfp, vmaddfp128 and vmaddcfp128. */
  VectorMath vnmsubfp = VectorMath::kFast;
  VectorMath vmsum3fp128 = VectorMath::kFast;
  VectorMath vmsum4fp128 = VectorMath::kFast;
  VectorMath vrefp = VectorMath::kFast;
  VectorMath vrsqrtefp = VectorMath::kFast;
  VectorMath vexptefp = VectorMath::kFast;
  VectorMath fres = VectorMath::kFast;
  VectorMath frsqrte = VectorMath::kFast;
};

/** Settings that control how a binary is analyzed and emitted, loaded from TOML. */
struct RecompilerConfig {
  RecompilerConfig() = default;
  RecompilerConfig(const RecompilerConfig&) = default;
  RecompilerConfig& operator=(const RecompilerConfig&) = default;
  RecompilerConfig(RecompilerConfig&&) noexcept(false) = default;
  RecompilerConfig& operator=(RecompilerConfig&&) noexcept(false) = default;
  ~RecompilerConfig() = default;

  /**
   * TOML key `project_name`, default `rex`. Prefix of the generated `<name>_*.h` and `.cpp` files
   * and the `project` template variable.
   */
  std::string project_name = "rex";
  std::string file_path; /**< Path to XEX or ELF file */
  /**
   * TOML key `out_directory_path`, default empty. Directory the generated files are written to,
   * relative to the config file directory.
   */
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

  /**
   * TOML key `skip_lr`, default false. When true, omits the `ctx.lr` store at calls between title
   * functions and the `mflr` read. Calls to imports always store `ctx.lr`.
   */
  bool skip_lr = false;
  /**
   * TOML key `ctr_as_local`, default false. When true, keeps CTR in a local variable instead of
   * `ctx.ctr`.
   */
  bool ctr_as_local_variable = false;
  /**
   * TOML key `xer_as_local`, default false. When true, keeps XER in a local variable instead of
   * `ctx.xer`.
   */
  bool xer_as_local_variable = false;
  /**
   * TOML key `reserved_as_local`, default false. When true, keeps the reservation register in a
   * local variable instead of `ctx.reserved`.
   */
  bool reserved_register_as_local_variable = false;
  /**
   * TOML key `skip_msr`, default false. When true, omits the global lock check at mfmsr, and the
   * MSR update and the global lock enter and leave at mtmsrd.
   */
  bool skip_msr = false;
  /**
   * TOML key `cr_as_local`, default false. When true, keeps the CR fields in local variables
   * instead of `ctx.cr0` to `ctx.cr7`.
   */
  bool cr_registers_as_local_variables = false;
  /**
   * TOML key `non_argument_as_local`, default false. When true, keeps r0, r11, r12, f0 and v32 to
   * v63 in local variables. r2 and r13 always stay in ctx.
   */
  bool non_argument_registers_as_local_variables = false;
  /**
   * TOML key `non_volatile_as_local`, default false. When true, keeps r14 and above, f14 and above
   * and the high vector registers in local variables, except in register sharing functions and SEH
   * scopes.
   */
  bool non_volatile_registers_as_local_variables = false;
  /**
   * TOML key `forward_stack_stores`, default false. When true, a load through r1 reads the value
   * an earlier store through r1 of the same offset and width left in the same basic block, instead
   * of reading guest memory.
   */
  bool forward_stack_stores = false;
  bool generate_exception_handlers = false;

  VectorMathConfig vector_math; /**< TOML table `[vector_math]`. */

  /**
   * Overrides the DLL module flag. When unset the orchestrator infers it from the module
   * position in the manifest, with the entrypoint false and modules true.
   */
  std::optional<bool> is_dll;

  /**
   * TOML tables `[functions]` keyed by hex entry address. Overrides the size, end, name and parent
   * of discovered functions.
   */
  std::unordered_map<u32, FunctionConfig> functions;
  /**
   * TOML array `[[switch_tables]]` keyed by the bctr address. Manual jump tables that replace
   * automatic detection.
   */
  std::unordered_map<u32, JumpTable> switch_tables;
  /**
   * TOML array `[[midasm_hook]]` keyed by instruction address. Calls to user functions inserted
   * into the generated code.
   */
  std::unordered_map<u32, MidAsmHook> mid_asm_hooks;
  /**
   * TOML key `longjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_longjmp`.
   */
  u32 longjmp_address = 0;
  /**
   * TOML key `setjmp_address`, default 0. Function address whose calls are emitted as
   * `ppc_setjmp`.
   */
  u32 setjmp_address = 0;

  /** Shim function addresses keyed by registry name, read from the [shim] TOML table. */
  std::unordered_map<std::string, u32> shim_functions;

  std::unordered_map<u32, u32> invalid_instruction_hints; /**< Maps address to size */
  /** Loads and stores the [mmio.gpu] and [mmio.apu] tables route to a register block. */
  std::unordered_map<u32, MMIODeclaredAccess> declared_mmio_accesses;
  /**
   * TOML array `analysis.exception_handler_funcs`. Entry addresses analysis treats as exception
   * handler functions, sorted and deduplicated after load.
   */
  std::vector<u32> exception_handler_func_hints;

  /**
   * Loads configuration from a TOML file. An optional `includes` array layers other files, with
   * paths relative to the including file. Scalars take the last value, keyed tables are additive
   * with the last value winning on a repeated key, arrays of tables are deduplicated by primary
   * key and sets are additive.
   *
   * @return True on success, false on parse failure, circular include or depth exceeded
   */
  bool Load(const std::string_view& config_file_path);

  /**
   * Loads configuration from an in memory TOML table such as an inline binary entry inside a
   * manifest. Includes referenced from the table resolve relative to `base_dir`. Merges as
   * Load() does.
   *
   * @param base_dir Directory that includes resolve against
   * @param source File and section the table came from, shown at the start of key warnings
   */
  bool LoadFromTable(const toml::table& tbl, const std::filesystem::path& base_dir,
                     const std::string& source = "<inline>");

  /**
   * Loads a table that several binaries share, such as the top level keys of a manifest. Applies
   * the table and its includes as LoadFromTable() does, but skips the finalize checks because the
   * shared layer is not a complete binary config.
   *
   * @param base_dir Directory that includes resolve against
   * @param source File and section the table came from, shown at the start of key warnings
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
   * @param base First address of the binary image.
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

  /** Reports whether any configured shim function requires the shim heap. */
  bool UsesShimHeap() const;

  struct ValidationResult {
    bool valid = true; /**< True when there are no errors, warnings allowed */
    /** Non fatal issues such as an empty `file_path` or `out_directory_path`. */
    std::vector<std::string> warnings;
    std::vector<std::string> errors; /**< Fatal issues that block codegen */

    /** Reports whether validation found no errors. */
    explicit operator bool() const { return valid; }
  };

  ValidationResult Validate() const;
};

}  // namespace rex::codegen
