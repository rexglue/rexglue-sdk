#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <dlfcn.h>

#include <cstdio>
#include <string>

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

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  Close();
  int flags = (mode == SymbolResolution::kImmediate) ? RTLD_NOW : RTLD_LAZY;
  handle_ = dlopen(path.c_str(), flags);
  if (!handle_ && !path.has_parent_path() && !path.has_extension()) {
    // Codegen registers recompiled guest DLLs by their bare CMake target name
    // ("game_level"). LoadLibrary on Windows appends ".dll" by itself; here
    // the file is "libgame_level.so" / ".dylib", so try that name too. It is
    // looked up through the caller's RUNPATH ($ORIGIN), next to the runtime.
#if REX_PLATFORM_MAC
    const std::string alt = "lib" + path.string() + ".dylib";
#else
    const std::string alt = "lib" + path.string() + ".so";
#endif
    handle_ = dlopen(alt.c_str(), flags);
  }
  if (!handle_) {
    if (const char* err = dlerror()) {
      std::fprintf(stderr, "dlopen(%s) failed: %s\n", path.c_str(), err);
    }
  }
  return handle_ != nullptr;
}

void DynamicLibrary::Close() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
  return dlsym(handle_, name);
}

}  // namespace rex::platform
