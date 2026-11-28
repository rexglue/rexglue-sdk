/**
 * @file        kernel/title_debug_output.h
 * @brief       The debug text a title prints, read from printf style calls and logged as lines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <rex/logging/log_category.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::kernel {

/** Encoding of narrow text handed to ITitleDebugOutput::Write. */
enum class TextEncoding : u8 {
  kUTF8,     /**< UTF-8. Bytes that are not valid UTF-8 are logged as U+FFFD. */
  kShiftJIS, /**< Code page 932. */
};

/** Error::code of a failed PrintfCall::Format. */
enum class PrintfError : u8 {
  kNoMemory = 1,       /**< The context belongs to no thread state with guest memory. */
  kUnreadableFormat,   /**< The format pointer is 0 or the title could not read it. */
  kUnterminatedFormat, /**< No terminator within PrintfCall::kMaxFormatLength bytes. */
  kUnreadableArgument, /**< A string or stack argument is not readable. */
  kInvalidFormat,      /**< The engine rejected the format. */
};

/**
 * A printf style call a guest function received, read from its register file. Formatting reads
 * guest memory only where the title could read it and writes none, so a hook can format a call
 * and then run the original function.
 *
 * @code
 * REX_EXTERN(__imp__ExamplePrint);
 *
 * REX_HOOK_RAW(ExamplePrint) {
 *   auto& out = rex::kernel::GetCurrent().GetDebugOutput();
 *   if (out.IsEnabled()) {
 *     if (auto text = rex::kernel::PrintfCall::FromVarArgs(ctx, 1).Format()) {
 *       out.Write(*text, out.GetEncoding());
 *       out.Flush();
 *     }
 *   }
 *   __imp__ExamplePrint(ctx, base);
 * }
 * @endcode
 */
class REX_KERNEL_API PrintfCall {
 public:
  /** The most bytes Format reads for the format string, its terminator included. */
  static constexpr size_t kMaxFormatLength = 4096;

  /** The default bound of the formatted text in bytes. */
  static constexpr size_t kMaxLength = 4096;

  /**
   * Describes a variadic call. The arguments follow the format in r3 to r10, then in the stack
   * slots of the caller.
   *
   * @param ctx The register file at the entry of the function, which must outlive the result.
   * @param format_index The argument index of the format pointer, 0 for r3.
   * @return The call.
   */
  static PrintfCall FromVarArgs(const PPCContext& ctx, u32 format_index);

  /**
   * Describes a call that passes a va_list.
   *
   * @param ctx The register file at the entry of the function, which must outlive the result.
   * @param format_index The argument index of the format pointer, 0 for r3.
   * @param list_index The argument index of the guest va_list pointer.
   * @return The call.
   */
  static PrintfCall FromVaList(const PPCContext& ctx, u32 format_index, u32 list_index);

  /**
   * Formats the call. Reads guest memory only where the title could read it and writes none. A %n
   * conversion consumes its argument and stores nothing.
   *
   * @param max_length The most bytes of text to return. Longer text is cut and still succeeds.
   * @return The text in the encoding of the title, or an error of category kValidation whose code
   * is a PrintfError.
   */
  [[nodiscard]] Result<std::string> Format(size_t max_length = kMaxLength) const;

 private:
  PrintfCall(const PPCContext& ctx, u32 format_index, u32 list_index, bool has_list)
      : ctx_(&ctx), format_index_(format_index), list_index_(list_index), has_list_(has_list) {}

  const PPCContext* ctx_;
  u32 format_index_;
  u32 list_index_;
  bool has_list_;
};

/**
 * Receives the debug text of a title and logs it as whole lines at info level, under the kernel
 * category unless the title sets another.
 */
class ITitleDebugOutput {
 public:
  virtual ~ITitleDebugOutput() = default;

  /**
   * Sets the category every line goes to. Call it before guest threads start, such as in
   * TitleApp::OnTitleLaunch. A line that a thread ends while the category changes goes to the old
   * or the new category.
   *
   * @param category The category, added to the logger and valid until the title unloads.
   *
   * @code
   * REXLOG_CATEGORY(example, "Debug text of the example title")
   *
   * rex::Result<void> ExampleApp::OnTitleLaunch(rex::kernel::TitleRuntime& title) {
   *   title.GetDebugOutput().SetCategory(rex::log::example);
   *   return rex::Ok();
   * }
   * @endcode
   */
  virtual void SetCategory(const rex::log::LogCategory& category) = 0;

  /** Returns the category every line goes to, the kernel category by default. */
  virtual const rex::log::LogCategory& GetCategory() const = 0;

  /** True when a line would be logged. Check it before reading or formatting text. */
  virtual bool IsEnabled() const = 0;

  /** Returns the encoding the SDK uses for narrow text of the title. */
  virtual TextEncoding GetEncoding() const = 0;

  /**
   * Appends text to the line of the calling thread and logs every line it completes. A line ends
   * at a line feed, and a line longer than the line limit is logged in parts.
   *
   * @param text The text, which may hold several lines or part of one.
   * @param encoding The encoding of text.
   */
  virtual void Write(std::string_view text, TextEncoding encoding) = 0;

  /** Logs the unfinished line of the calling thread, if any. */
  virtual void Flush() = 0;
};

}  // namespace rex::kernel
