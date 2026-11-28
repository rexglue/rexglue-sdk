#include "kernel/title_debug_output.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rex/kernel/kernel_settings.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_debug_output.h>
#include <rex/logging/log_category.h>
#include <rex/ppc/call.h>
#include <rex/string/text.h>

#include "kernel/categories.h"
#include "kernel/kernel.h"
#include "kernel/title_state.h"
#include "kernel/xboxkrnl/format.h"

namespace rex::kernel {

namespace {

constexpr size_t kMaxLineLength = 1024;
constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

struct DebugLine {
  std::string text;
  char lead = 0;
};

thread_local DebugLine t_line;

bool IsShiftJISLead(char c) {
  const auto byte = static_cast<u8>(c);
  return (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC);
}

bool IsContinuation(char c) {
  return (static_cast<u8>(c) & 0xC0) == 0x80;
}

size_t GetSequenceLength(char lead) {
  const auto byte = static_cast<u8>(lead);
  if (byte >= 0xF0) {
    return 4;
  }
  if (byte >= 0xE0) {
    return 3;
  }
  if (byte >= 0xC0) {
    return 2;
  }
  return 1;
}

size_t GetValidSequenceLength(std::string_view text, size_t at) {
  const auto lead = static_cast<u8>(text[at]);
  if (lead < 0x80) {
    return 1;
  }
  u8 low = 0x80;
  u8 high = 0xBF;
  size_t length = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    low = lead == 0xE0 ? 0xA0 : 0x80;
    high = lead == 0xED ? 0x9F : 0xBF;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    low = lead == 0xF0 ? 0x90 : 0x80;
    high = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return 0;
  }
  if (text.size() - at < length) {
    return 0;
  }
  const auto second = static_cast<u8>(text[at + 1]);
  if (second < low || second > high) {
    return 0;
  }
  for (size_t i = 2; i < length; ++i) {
    if (!IsContinuation(text[at + i])) {
      return 0;
    }
  }
  return length;
}

std::string ReplaceInvalidUTF8(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  size_t at = 0;
  while (at < text.size()) {
    const size_t length = GetValidSequenceLength(text, at);
    if (length == 0) {
      result.append(kReplacementCharacter);
      ++at;
    } else {
      result.append(text.substr(at, length));
      at += length;
    }
  }
  return result;
}

void LogLine(const rex::log::LogCategory& category, std::string_view line) {
  while (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return;
  }
  REXLOG_CAT_INFO(category, "{}", ReplaceInvalidUTF8(line));
}

size_t GetCutLength(std::string_view text) {
  size_t start = text.size();
  for (size_t back = 1; back <= 4 && back <= text.size(); ++back) {
    if (!IsContinuation(text[text.size() - back])) {
      start = text.size() - back;
      break;
    }
  }
  if (start == text.size() || start == 0 || GetSequenceLength(text[start]) <= text.size() - start) {
    return text.size();
  }
  return start;
}

void AppendUTF8(const rex::log::LogCategory& category, DebugLine& line, std::string_view text) {
  for (const char c : text) {
    if (c == '\n') {
      LogLine(category, line.text);
      line.text.clear();
      continue;
    }
    line.text.push_back(c);
    if (line.text.size() >= kMaxLineLength) {
      const size_t cut = GetCutLength(line.text);
      LogLine(category, std::string_view(line.text).substr(0, cut));
      line.text.erase(0, cut);
    }
  }
}

Result<std::string> Fail(PrintfError code, std::string message) {
  return Err<std::string>(ErrorCategory::kValidation, std::move(message), static_cast<int>(code));
}

std::optional<u64> ReadArgument(const PPCContext& ctx, const MemoryView& view, u32 index) {
  if (index < ppc::ArgTranslator::kIntegerRegisters) {
    return ppc::ArgTranslator::GetIntegerRegister(const_cast<PPCContext&>(ctx), index).u64;
  }
  return view.TryLoad<u64>(ppc::ArgTranslator::GetStackSlotAddress(ctx, index));
}

}  // namespace

PrintfCall PrintfCall::FromVarArgs(const PPCContext& ctx, u32 format_index) {
  return PrintfCall(ctx, format_index, 0, false);
}

PrintfCall PrintfCall::FromVaList(const PPCContext& ctx, u32 format_index, u32 list_index) {
  return PrintfCall(ctx, format_index, list_index, true);
}

Result<std::string> PrintfCall::Format(size_t max_length) const {
  const ThreadState* thread = ctx_->thread_state ? ctx_->thread_state : ThreadState::Get();
  const GuestAddressSpace* memory = thread ? thread->GetAddressSpace() : nullptr;
  if (!memory) {
    return Fail(PrintfError::kNoMemory, "the call belongs to no thread with guest memory");
  }
  const MemoryView view(*memory);

  const std::optional<u64> format_argument = ReadArgument(*ctx_, view, format_index_);
  if (!format_argument) {
    return Fail(PrintfError::kUnreadableFormat,
                std::format("the stack slot of argument {} is not readable", format_index_));
  }
  const auto format_address = static_cast<u32>(*format_argument);
  const std::optional<std::string_view> format =
      view.TryGetCString(format_address, kMaxFormatLength);
  if (!format) {
    return Fail(PrintfError::kUnreadableFormat,
                std::format("the format at 0x{:08X} is not readable", format_address));
  }
  const u64 end = static_cast<u64>(format_address) + format->size();
  if (format->size() == kMaxFormatLength || end > 0xFFFFFFFFull ||
      view.TryLoad<u8>(static_cast<u32>(end)).value_or(1) != 0) {
    return Fail(PrintfError::kUnterminatedFormat,
                std::format("the format at 0x{:08X} has no terminator within {} bytes",
                            format_address, kMaxFormatLength));
  }
  const std::string format_text(*format);

  xboxkrnl::detail::BoundedStringFormatData data(format_text, max_length);
  xboxkrnl::detail::CheckedMemory checked(view);
  i32 count = 0;
  bool unreadable = false;
  if (has_list_) {
    const std::optional<u64> list = ReadArgument(*ctx_, view, list_index_);
    if (!list) {
      return Fail(PrintfError::kUnreadableArgument,
                  std::format("the stack slot of argument {} is not readable", list_index_));
    }
    xboxkrnl::detail::CheckedArrayArgList args(view, static_cast<u32>(*list));
    count = xboxkrnl::detail::FormatCore(checked, data, args, false);
    unreadable = args.IsUnreadable();
  } else {
    xboxkrnl::detail::CheckedStackArgList args(*ctx_, view, format_index_ + 1);
    count = xboxkrnl::detail::FormatCore(checked, data, args, false);
    unreadable = args.IsUnreadable();
  }
  if (unreadable || checked.IsUnreadable()) {
    return Fail(
        PrintfError::kUnreadableArgument,
        std::format("an argument of the format at 0x{:08X} is not readable", format_address));
  }
  if (count < 0 && !data.IsTruncated()) {
    return Fail(PrintfError::kInvalidFormat,
                std::format("the format at 0x{:08X} is not valid", format_address));
  }
  return std::move(data.GetOutput());
}

void TitleState::SetCategory(const rex::log::LogCategory& category) {
  debug_category_.store(&category, std::memory_order_release);
}

const rex::log::LogCategory& TitleState::GetCategory() const {
  const rex::log::LogCategory* category = debug_category_.load(std::memory_order_acquire);
  return category != nullptr ? *category : rex::log::kernel;
}

bool TitleState::IsEnabled() const {
  return GetCategory().IsEnabled(rex::log::Level::kInfo);
}

TextEncoding TitleState::GetEncoding() const {
  return GetKernelSettings().debug_text_encoding.Get() == "shift_jis" ? TextEncoding::kShiftJIS
                                                                      : TextEncoding::kUTF8;
}

void TitleState::Write(std::string_view text, TextEncoding encoding) {
  DebugLine& line = t_line;
  const rex::log::LogCategory& category = GetCategory();
  if (!category.IsEnabled(rex::log::Level::kInfo)) {
    if (!line.text.empty() || line.lead != 0) {
      line.text.clear();
      line.lead = 0;
    }
    return;
  }
  if (encoding == TextEncoding::kShiftJIS) {
    std::string bytes;
    if (line.lead != 0) {
      bytes.push_back(line.lead);
      line.lead = 0;
    }
    bytes.append(text);
    size_t at = 0;
    while (at < bytes.size()) {
      if (!IsShiftJISLead(bytes[at])) {
        ++at;
      } else if (at + 1 < bytes.size()) {
        at += 2;
      } else {
        line.lead = bytes[at];
        bytes.pop_back();
        break;
      }
    }
    AppendUTF8(category, line, rex::string::ShiftJISToUTF8(bytes));
    return;
  }
  if (line.lead != 0) {
    line.lead = 0;
    AppendUTF8(category, line, kReplacementCharacter);
  }
  AppendUTF8(category, line, text);
}

void TitleState::Flush() {
  DebugLine& line = t_line;
  const rex::log::LogCategory& category = GetCategory();
  if (line.lead != 0) {
    line.lead = 0;
    AppendUTF8(category, line, kReplacementCharacter);
  }
  if (!line.text.empty()) {
    LogLine(category, line.text);
    line.text.clear();
  }
}

void WritePrintf(const PPCContext& ctx, u32 format_index) {
  ITitleDebugOutput& output = GetCurrentState();
  if (!output.IsEnabled()) {
    return;
  }
  if (auto text = PrintfCall::FromVarArgs(ctx, format_index).Format()) {
    output.Write(*text, output.GetEncoding());
  }
}

}  // namespace rex::kernel
