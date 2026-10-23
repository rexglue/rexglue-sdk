#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <climits>
#include <string>

#include <rex/string/utf8.h>

namespace rex {

namespace {

constexpr UINT kShiftJIS = 932;

std::wstring DecodeToWide(UINT code_page, const std::string_view source) {
  if (source.empty() || source.size() > INT_MAX) {
    return {};
  }
  const int length = static_cast<int>(source.size());
  const int required = MultiByteToWideChar(code_page, 0, source.data(), length, nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(code_page, 0, source.data(), length, result.data(), required);
  return result;
}

std::string EncodeFromWide(UINT code_page, const std::wstring_view source) {
  if (source.empty() || source.size() > INT_MAX) {
    return {};
  }
  const int length = static_cast<int>(source.size());
  const int required =
      WideCharToMultiByte(code_page, 0, source.data(), length, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(code_page, 0, source.data(), length, result.data(), required, nullptr,
                      nullptr);
  return result;
}

}  // namespace

std::wstring ToWideString(const std::string_view source) {
  return DecodeToWide(CP_UTF8, source);
}

std::string ToUTF8(const std::wstring_view source) {
  return EncodeFromWide(CP_UTF8, source);
}

std::string ShiftJISToUTF8(const std::string_view source) {
  return EncodeFromWide(CP_UTF8, DecodeToWide(kShiftJIS, source));
}

std::string UTF8ToShiftJIS(const std::string_view source) {
  return EncodeFromWide(kShiftJIS, DecodeToWide(CP_UTF8, source));
}

}  // namespace rex
