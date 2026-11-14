/**
 * @file        platform/dynlib.h
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
#include <span>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::platform {

enum class SymbolResolution : u8 {
  kLazy,      /**< Resolve on first use. RTLD_LAZY on POSIX. */
  kImmediate, /**< Resolve all at load. RTLD_NOW on POSIX, the Windows behavior. */
};

/** Loads a shared library and resolves symbols from it. Move only. */
class REX_API DynamicLibrary : public rex::MoveOnly {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  /**
   * Loads the library at the path, closing any held library.
   *
   * @param mode Symbol resolution mode. Informational on Windows.
   * @return True on success. On failure GetLastError() holds the OS text.
   */
  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);

  /**
   * Loads a library target from the folder rexbase was loaded from, or else from the executable
   * folder, adding the platform prefix and extension. Closes any held library.
   */
  bool LoadNamed(std::string_view name, SymbolResolution mode = SymbolResolution::kLazy);

  /**
   * Opens the library only when the process has it mapped. Closes any held library.
   *
   * @param path Full path or file name of the library.
   * @return True when the library was mapped. It is never loaded.
   */
  bool Attach(const std::filesystem::path& path);

  /** Closes the library. Safe when nothing is loaded. */
  void Close();

  explicit operator bool() const { return handle_ != nullptr; }

  /**
   * Returns the address of an exported symbol, or null when the library is not loaded or has no
   * such symbol.
   */
  void* GetRawSymbol(const char* name) const;

  /**
   * Typed form of GetRawSymbol.
   *
   * @return The symbol cast to T, or null.
   */
  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

  /** Returns the OS error text of the last failed Load, empty after a success. */
  const std::string& GetLastError() const { return last_error_; }

 private:
  void* handle_ = nullptr;
  std::string last_error_;
};

#if REX_PLATFORM_WIN32
/**
 * Finds a section of a loaded image by name.
 *
 * @param name The section name, at most 8 characters.
 * @return The section bytes, empty when the image has no such section.
 */
REX_API std::span<const u8> FindImageSection(const void* image, std::string_view name);
#endif

}  // namespace rex::platform
