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

#include <rex/export.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Controls when a loaded library's imported symbols are resolved.
 */
enum class SymbolResolution : u8 {
  kLazy,      /**< Resolve on first use. RTLD_LAZY on POSIX. */
  kImmediate, /**< Resolve all at load. RTLD_NOW on POSIX, the Windows behavior. */
};

/**
 * Loads a shared library and resolves symbols from it. Move only.
 */
class REX_API DynamicLibrary : public rex::MoveOnly {
 public:
  DynamicLibrary() = default;
  /** Closes the library when one is loaded. */
  ~DynamicLibrary();

  /**
   * Takes ownership of the library held by another object.
   *
   * @param other The object to take from, left empty.
   */
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  /**
   * Closes the library held, then takes ownership of the library held by another object.
   *
   * @param other The object to take from, left empty.
   * @return This object.
   */
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  /**
   * Loads the library at the path, closing any library held before.
   *
   * @param path Full path to the library file.
   * @param mode Symbol resolution mode. Informational on Windows.
   * @return True on success. On failure GetLastError() holds the OS text.
   */
  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);

  /**
   * Opens the library only when the process has it mapped, closing any library held
   * before.
   *
   * @param path Full path or file name of the library.
   * @return True when the process had the library mapped. A library that is not mapped is never
   *         loaded.
   */
  bool Attach(const std::filesystem::path& path);

  /**
   * Closes the library. Safe when nothing is loaded.
   */
  void Close();

  /**
   * @return True when a library is loaded.
   */
  explicit operator bool() const { return handle_ != nullptr; }

  /**
   * @param name Exported symbol name.
   * @return The symbol address, or null when the library is not loaded or has no such symbol.
   */
  void* GetRawSymbol(const char* name) const;

  /**
   * Typed form of GetRawSymbol.
   *
   * @tparam T Function pointer or pointer type of the symbol.
   * @param name Exported symbol name.
   * @return The symbol cast to T, or null when the library is not loaded or has no such symbol.
   */
  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

  /**
   * @return The OS error text of the last failed Load, empty after a successful Load.
   */
  const std::string& GetLastError() const { return last_error_; }

 private:
  void* handle_ = nullptr;
  std::string last_error_;
};

#if REX_PLATFORM_WIN32
/**
 * Finds a section of a loaded image by name.
 *
 * @param image The base address of the image.
 * @param name The section name, at most 8 characters.
 * @return The section bytes, empty when the image has no such section.
 */
REX_API std::span<const u8> FindImageSection(const void* image, std::string_view name);
#endif

}  // namespace rex::platform
