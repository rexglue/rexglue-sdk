#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "platform_win.h"

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_), last_error_(std::move(other.last_error_)) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    last_error_ = std::move(other.last_error_);
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution /*mode*/) {
  // Windows resolves all imports at load time, so mode has no effect.
  Close();
  handle_ = static_cast<void*>(LoadLibraryW(path.c_str()));
  if (!handle_) {
    DWORD code = GetLastError();
    char* text = nullptr;
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    if (length && text) {
      last_error_.assign(text, length);
      while (!last_error_.empty() && (last_error_.back() == '\n' || last_error_.back() == '\r')) {
        last_error_.pop_back();
      }
      LocalFree(text);
    } else {
      last_error_ = "error " + std::to_string(code);
    }
    return false;
  }
  last_error_.clear();
  return true;
}

void DynamicLibrary::Close() {
  if (handle_) {
    FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
}

std::span<const u8> FindImageSection(const void* image, std::string_view name) {
  const auto* base = static_cast<const u8*>(image);
  const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const std::span sections(IMAGE_FIRST_SECTION(nt), nt->FileHeader.NumberOfSections);
  for (const IMAGE_SECTION_HEADER& section : sections) {
    const auto* text = reinterpret_cast<const char*>(section.Name);
    if (std::string_view(text, strnlen(text, IMAGE_SIZEOF_SHORT_NAME)) == name) {
      return {base + section.VirtualAddress, section.Misc.VirtualSize};
    }
  }
  return {};
}

}  // namespace rex::platform
