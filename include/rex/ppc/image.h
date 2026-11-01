/**
 * @file        ppc/image.h
 * @brief       PPC image layout, embedded metadata assets and pdata decoding
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <rex/types.h>
#include <rex/platform.h>

struct PPCFuncMapping;

namespace rex::kernel {
class IModuleRegistry;
}  // namespace rex::kernel

namespace rex::ppc {

/**
 * Registers the recompiled libraries of a project with several binaries.
 *
 * @param modules The module registry of the runtime being launched.
 */
using RegisterModulesFunc = void (*)(kernel::IModuleRegistry& modules);

/** Returns the recompiled code that codegen generated for an image. */
using RecompiledCodeFunc = std::span<const u8> (*)();

/** Codegen options that change how the generated code treats registers and state. */
struct PPCCodegenFlags {
  /** Omits the `ctx.lr` store and `mflr` read between guest functions. Imports store it. */
  bool skip_lr = false;
  /** Keeps CTR in a local variable instead of `ctx.ctr`. Config key `ctr_as_local`. */
  bool ctr_as_local = false;
  /** Keeps XER in a local variable instead of `ctx.xer`. Config key `xer_as_local`. */
  bool xer_as_local = false;
  /** Keeps the reservation register in a local variable instead of `ctx.reserved`. */
  bool reserved_as_local = false;
  /** Omits the generated global lock check that mfmsr emits. Config key `skip_msr`. */
  bool skip_msr = false;
  /** Keeps the CR fields in local variables instead of `ctx.cr0` to `ctx.cr7`. */
  bool cr_as_local = false;
  /** Keeps r0, r11, r12, f0 and v32 to v63 in local variables. */
  bool non_argument_as_local = false;
  /** Keeps r14 and above, f14 and above and the high vector registers in locals. */
  bool non_volatile_as_local = false;
};

/** The argument registers a recompiled function reads at its entry. */
struct PPCCallUsage {
  u32 address;
  u8 gpr_mask;  /**< Bit n is set when the function reads r(3 + n), r3 to r10. */
  u16 fpr_mask; /**< Bit n is set when the function reads f(1 + n), f1 to f13. */
};

/** PPC image layout passed from the generated config header into TitleApp::Init. */
struct PPCImage {
  u32 code_base; /**< First guest address of the generated code, given to the function table. */
  u32 code_size;
  u32 image_base;
  u32 image_size;
  /** Guest address and host function pairs registered at startup, ended by guest address 0. */
  const PPCFuncMapping* func_mappings;
  bool shim_heap = false; /**< Set by codegen when [shim] holds the heap functions. */
  RegisterModulesFunc register_modules = nullptr; /**< Set by codegen for multi binary projects. */
  PPCCodegenFlags codegen_flags{};                /**< Set by codegen from the config flags. */
  RecompiledCodeFunc recompiled_code =
      nullptr; /**< Set by codegen to the generated recompiled code. */
  /** Set by codegen to the argument registers each function reads, ended by address 0. */
  const PPCCallUsage* call_usage = nullptr;
};

/** An embedded metadata asset found by path. */
struct EmbeddedMetadataAsset {
  std::span<const u8> bytes;
};

/**
 * Registers a metadata asset embedded into the host executable.
 *
 * An empty path, null data, 0 size or a path that normalizes to nothing is rejected.
 *
 * @param relative_path Path relative to the metadata root, such as "icons/custom/67.png".
 * @param data Must stay valid for the whole process.
 */
REX_PPC_API bool RegisterEmbeddedMetadataAsset(std::string_view relative_path, const u8* data,
                                               std::size_t size);

/**
 * Looks up an embedded metadata asset by normalized path.
 *
 * @return The asset, or an empty optional when none is registered at the path.
 */
REX_PPC_API std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadataAsset(
    const std::filesystem::path& relative_path);

/** Function type held in the 2 high bits of a pdata word. */
enum class RuntimeFunctionType : u8 {
  kSaveMillicode,    /**< Stores registers and returns. */
  kNoHandler,        /**< A normal function. */
  kRestoreMillicode, /**< Loads registers and returns. */
  kHandler           /**< A function with handler data before it. */
};

struct RuntimeFunction {
  u32 begin; /**< Address of the first instruction. */
  RuntimeFunctionType type;
  u32 length;   /**< Instruction count including the prologue. */
  u32 prologue; /**< Instruction count of the prologue. */
};

/**
 * Decodes a pdata entry.
 *
 * @param begin The begin address word.
 * @param word The type, length and prologue word.
 */
constexpr RuntimeFunction DecodeRuntimeFunction(u32 begin, u32 word) {
  return RuntimeFunction{begin, static_cast<RuntimeFunctionType>(word >> 30),
                         (word >> 8) & 0x3FFFFFu, word & 0xFFu};
}

}  // namespace rex::ppc
