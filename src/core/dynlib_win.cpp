#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution /*mode*/) {
  // Windows resolves all imports at load time; mode is informational only.
  Close();
  if (path.is_absolute()) {
    // A library given by its full path (the GPU plugin next to the
    // executable) finds the DLLs it needs in its own folder first, whatever
    // the current directory is (a launcher or Wine may start the program from
    // elsewhere).
    handle_ = static_cast<void*>(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    return handle_ != nullptr;
  }
  handle_ = static_cast<void*>(LoadLibraryW(path.c_str()));
  if (!handle_ && !path.has_parent_path() && !path.has_extension()) {
    // Codegen registers recompiled guest DLLs by their bare CMake target name
    // ("game_level"), which LoadLibrary completes to "game_level.dll". A
    // MinGW build names the file "libgame_level.dll", so try that too.
    // (The POSIX loader does the same for "lib<name>.so".)
    const std::wstring alt = L"lib" + path.wstring() + L".dll";
    handle_ = static_cast<void*>(LoadLibraryW(alt.c_str()));
  }
  // On failure GetLastError() still tells the caller why; nothing is called
  // in between.
  return handle_ != nullptr;
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

}  // namespace rex::platform
