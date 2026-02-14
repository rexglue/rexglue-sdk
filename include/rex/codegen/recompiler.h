/**
 * @file        rexcodegen/internal/recompiler.h
 * @brief       PPC to C++ recompiler interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/codegen/recompiled_function.h>

// Forward declarations
struct ppc_insn;

namespace rex {
class Runtime;

namespace codegen {

// Forward declare Function from recompiled_function.h
struct Function;

struct RecompilerLocalVariables {
  bool ctr{};
  bool xer{};
  bool reserved{};
  bool cr[8]{};
  bool r[32]{};
  bool f[32]{};
  bool v[128]{};
  bool env{};
  bool temp{};
  bool v_temp{};
  bool ea{};

  /// Tracks which GPRs contain MMIO base addresses (bit N = rN is MMIO base)
  /// Set when lis loads a value with upper 16 bits >= 0x7F00 (address >= 0x7F000000)
  /// or when oris sets upper bits >= 0xC800 (address >= 0xC8000000)
  uint32_t mmio_base_regs{0};

  void set_mmio_base(size_t reg) {
    if (reg < 32)
      mmio_base_regs |= (1u << reg);
  }
  void clear_mmio_base(size_t reg) {
    if (reg < 32)
      mmio_base_regs &= ~(1u << reg);
  }
  bool is_mmio_base(size_t reg) const { return reg < 32 && (mmio_base_regs & (1u << reg)); }
};

enum class CSRState { Unknown, FPU, VMX };

struct Recompiler {
  // Enforce In-order Execution of I/O constant for quick comparison
  // TODO: this shouldn't be here... find a new place for this EIEIO constant
  static constexpr uint32_t c_eieio = 0xAC06007C;

  std::unique_ptr<rex::Runtime> runtime;
  CodegenContext* ctx_ = nullptr;  // Non-owning pointer to context
  std::string out;
  size_t cppFileIndex = 0;

  // Deferred file writes - buffered until validation passes
  std::vector<std::pair<std::string, std::string>> pendingWrites;

  // Track if validation failed during Analyze()
  bool validationFailed_ = false;

  Recompiler();
  ~Recompiler();

  /// Append formatted text to output buffer.
  template <class... Args>
  void print(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
  }

  /// Append formatted text with newline to output buffer.
  template <class... Args>
  void println(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    out += '\n';
  }

  /// Recompile a single instruction (internal).
  bool recompile(const FunctionNode& fn, uint32_t base, const ppc_insn& insn, const uint32_t* data,
                 std::unordered_map<uint32_t, JumpTable>::iterator& switchTable,
                 RecompilerLocalVariables& localVariables, CSRState& csrState);

  /// Recompile an entire function (internal).
  bool recompile(const FunctionNode& fn);

  /**
   * Recompile all functions and write output.
   * Generated code will include SDK headers (rexglue/runtime/ppc_context.h).
   * @param force If true, generate output even if validation errors occur
   * @return true if output was generated, false if blocked due to errors
   */
  bool recompile(bool force);

  /**
   * Save current output buffer to pending writes.
   * @param name Optional custom filename; if empty, uses auto-generated name
   */
  void SaveCurrentOutData(const std::string_view& name = std::string_view());

  /// Write all pending files to disk (called after validation passes).
  void FlushPendingWrites();

 private:
  // Accessors for ctx_ members (convenience)
  FunctionGraph& graph() { return ctx_->graph; }
  const FunctionGraph& graph() const { return ctx_->graph; }
  const BinaryView& binary() const { return ctx_->binary(); }
  RecompilerConfig& config() { return ctx_->Config(); }
  const RecompilerConfig& config() const { return ctx_->Config(); }
  AnalysisState& analysisState() { return ctx_->analysisState(); }
  const AnalysisState& analysisState() const { return ctx_->analysisState(); }
};

}  // namespace codegen
}  // namespace rex