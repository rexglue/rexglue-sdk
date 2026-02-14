/**
 * @file        rex/codegen/code_emitter.h
 * @brief       Abstract code emission interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

#include <fmt/core.h>

namespace rex::codegen {

// Forward declarations
class RecompilerConfig;

/**
 * @brief CSR (Control/Status Register) state for FPU denormal handling.
 *
 * Tracks MXCSR configuration:
 * - Unknown: Initial or after function call
 * - Fpu: Denormals preserved (scalar FP)
 * - Vmx: Denormals flushed (vector FP)
 */
enum class CsrState {
  Unknown,  // Need to check/set on next FP instruction
  Fpu,      // Flush mode disabled (scalar FP)
  Vmx       // Flush mode enabled (vector FP)
};

/**
 * @brief Abstract interface for C++ code emission.
 *
 * Provides indentation management, formatted output, and CSR state tracking.
 * Implementations can write to strings, files, or streams.
 *
 * Example:
 * @code
 * StringEmitter emit;
 * emit.line("void foo() {{");
 * emit.indent();
 * emit.line("int x = {};", 42);
 * emit.dedent();
 * emit.line("}}");
 * @endcode
 */
class CodeEmitter {
 public:
  virtual ~CodeEmitter() = default;

  //=========================================================================
  // Indentation
  //=========================================================================

  /// Increase indentation level
  virtual void indent() = 0;

  /// Decrease indentation level
  virtual void dedent() = 0;

  /// Get current indentation string
  virtual std::string_view indentString() const = 0;

  //=========================================================================
  // Output
  //=========================================================================

  /// Write raw string (no indentation, no newline)
  virtual void raw(std::string_view text) = 0;

  /// Write formatted line with indentation and newline
  template <typename... Args>
  void line(fmt::format_string<Args...> fmt, Args&&... args) {
    raw(indentString());
    raw(fmt::format(fmt, std::forward<Args>(args)...));
    raw("\n");
  }

  /// Write empty line
  void blankLine() { raw("\n"); }

  /// Write comment line
  void comment(std::string_view text) {
    raw(indentString());
    raw("// ");
    raw(text);
    raw("\n");
  }

  //=========================================================================
  // CSR State Management
  //=========================================================================

  /// Get current CSR state
  CsrState csrState() const { return csrState_; }

  /// Set CSR state (called when mode is established)
  void setCsrState(CsrState state) { csrState_ = state; }

  /// Ensure CSR is in required state, emitting code if needed
  virtual void ensureCsrState(CsrState required);

  /// Reset CSR state to Unknown (call after function calls)
  void resetCsrState() { csrState_ = CsrState::Unknown; }

 protected:
  CsrState csrState_ = CsrState::Unknown;
};

/**
 * @brief CodeEmitter that writes to a string buffer.
 *
 * Useful for tests and building function bodies before output.
 */
class StringEmitter : public CodeEmitter {
 public:
  explicit StringEmitter(int indentWidth = 4) : indentWidth_(indentWidth) {}

  void indent() override {
    indentLevel_++;
    updateIndentString();
  }

  void dedent() override {
    if (indentLevel_ > 0)
      indentLevel_--;
    updateIndentString();
  }

  std::string_view indentString() const override { return indentStr_; }

  void raw(std::string_view text) override { buffer_.append(text); }

  /// Get the accumulated output
  const std::string& str() const { return buffer_; }

  /// Clear the buffer
  void clear() { buffer_.clear(); }

  /// Move the buffer out
  std::string take() { return std::move(buffer_); }

 private:
  void updateIndentString() { indentStr_ = std::string(indentLevel_ * indentWidth_, ' '); }

  std::string buffer_;
  std::string indentStr_;
  int indentLevel_ = 0;
  int indentWidth_ = 4;
};

}  // namespace rex::codegen
