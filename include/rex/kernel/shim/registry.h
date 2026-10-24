/**
 * @file        kernel/shim/registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <string_view>

#include <rex/bit.h>
#include <rex/types.h>

namespace rex::kernel::shim {

/** Functional group of a shim entry. */
enum class Group : u8 {
  kFile,      /**< File I/O backed by the filesystem module. */
  kHeap,      /**< The o1heap backed heap, all 4 entries are required together. */
  kMemory,    /**< Memory copy and fill routines. */
  kString,    /**< C, Win32 and UTF-16 string routines. */
  kMath,      /**< Floating point math and text conversion routines. */
  kDebug,     /**< Debug output routines. */
  kThreading, /**< Fiber routines. */
};

/** Behavior flags of a shim entry. */
enum class Flag : u8 {
  kNone = 0,              /**< No flags set. */
  kStub = 1 << 0,         /**< Returns success without doing the work. */
  kRequiresHeap = 1 << 1, /**< Needs the shim heap initialized. */
};

}  // namespace rex::kernel::shim

template <>
inline constexpr bool rex::is_flags_enum<rex::kernel::shim::Flag> = true;

namespace rex::kernel::shim {

/** Properties of a group. */
struct GroupTraits {
  Group id;                 /**< The group. */
  std::string_view name;    /**< Name used in the toml file and on the command line. */
  bool atomic;              /**< True when a partial selection is a configuration error. */
  std::string_view summary; /**< One line description. */
};

/** One shim entry. */
struct Entry {
  std::string_view name;        /**< Name without the hook prefix. */
  Group group;                  /**< The group the entry belongs to. */
  Flag flags;                   /**< Behavior flags. */
  std::string_view description; /**< One line description. */
};

/** Traits of every group. */
inline constexpr std::array<GroupTraits, 7> kGroups = {{
    {Group::kFile, "file", false, "File I/O backed by the filesystem module"},
    {Group::kHeap, "heap", true, "The o1heap backed heap, all 4 routines are required together"},
    {Group::kMemory, "memory", false, "Memory copy and fill routines"},
    {Group::kString, "string", false, "C, Win32 and UTF-16 string routines"},
    {Group::kMath, "math", false, "Floating point math and text conversion routines"},
    {Group::kDebug, "debug", false, "Debug output routines"},
    {Group::kThreading, "threading", false, "Fiber routines"},
}};

/** Every shim entry. */
inline constexpr std::array<Entry, 69> kRegistry = {{
    {"CreateFileA", Group::kFile, Flag::kNone, "Opens or creates a file"},
    {"ReadFile", Group::kFile, Flag::kNone, "Reads from an open file handle"},
    {"WriteFile", Group::kFile, Flag::kNone, "Writes to an open file handle"},
    {"SetFilePointer", Group::kFile, Flag::kNone, "Moves the file position with a 32 bit distance"},
    {"GetFileSize", Group::kFile, Flag::kNone, "Returns the low 32 bits of a file size"},
    {"GetFileSizeEx", Group::kFile, Flag::kNone, "Returns the 64 bit file size"},
    {"SetEndOfFile", Group::kFile, Flag::kNone, "Truncates a file at the current position"},
    {"FlushFileBuffers", Group::kFile, Flag::kNone, "Flushes a file"},
    {"DeleteFileA", Group::kFile, Flag::kNone, "Deletes a file"},
    {"CloseHandle", Group::kFile, Flag::kNone, "Closes a file or search handle"},
    {"FindFirstFileA", Group::kFile, Flag::kNone, "Begins a directory search"},
    {"FindNextFileA", Group::kFile, Flag::kNone, "Returns the next directory search result"},
    {"FindClose", Group::kFile, Flag::kNone, "Ends a directory search"},
    {"CreateDirectoryA", Group::kFile, Flag::kNone, "Creates a directory"},
    {"MoveFileA", Group::kFile, Flag::kNone, "Renames a file or directory"},
    {"SetFileAttributesA", Group::kFile, Flag::kNone, "Sets file attributes"},
    {"GetFileAttributesA", Group::kFile, Flag::kNone, "Returns file attributes"},
    {"GetFileAttributesExA", Group::kFile, Flag::kNone, "Returns extended file attribute data"},
    {"SetFilePointerEx", Group::kFile, Flag::kNone,
     "Moves the file position with a 64 bit distance"},
    {"SetFileTime", Group::kFile, Flag::kNone, "Sets the file timestamps"},
    {"CompareFileTime", Group::kFile, Flag::kNone, "Compares 2 file times"},
    {"CopyFileA", Group::kFile, Flag::kNone, "Copies a file"},
    {"RemoveDirectoryA", Group::kFile, Flag::kNone, "Removes an empty directory"},
    {"GetFileType", Group::kFile, Flag::kNone, "Returns the type of a file handle"},
    {"RtlAllocateHeap", Group::kHeap, Flag::kRequiresHeap,
     "Allocates memory, honors the zero flag"},
    {"RtlFreeHeap", Group::kHeap, Flag::kRequiresHeap, "Frees an allocation"},
    {"RtlSizeHeap", Group::kHeap, Flag::kRequiresHeap,
     "Returns the requested size of an allocation"},
    {"RtlReAllocateHeap", Group::kHeap, Flag::kRequiresHeap,
     "Resizes an allocation in place or by copy"},
    {"memcpy", Group::kMemory, Flag::kNone, "Copies memory"},
    {"memmove", Group::kMemory, Flag::kNone, "Copies overlapping memory"},
    {"memset", Group::kMemory, Flag::kNone, "Fills memory"},
    {"memchr", Group::kMemory, Flag::kNone, "Finds a byte in memory"},
    {"XMemCpy", Group::kMemory, Flag::kNone, "Copies memory"},
    {"XMemSet", Group::kMemory, Flag::kNone, "Fills memory"},
    {"XMemSet128", Group::kMemory, Flag::kNone, "Fills memory in 128 byte blocks"},
    {"memset_vmx", Group::kMemory, Flag::kNone, "Fills memory through the vector path"},
    {"memcpy_s", Group::kMemory, Flag::kNone, "Copies memory with a size check"},
    {"memmove_s", Group::kMemory, Flag::kNone, "Copies overlapping memory with a size check"},
    {"strncmp", Group::kString, Flag::kNone, "Compares 2 strings up to a length"},
    {"strncpy", Group::kString, Flag::kNone, "Copies a string up to a length"},
    {"strchr", Group::kString, Flag::kNone, "Finds a character in a string"},
    {"strstr", Group::kString, Flag::kNone, "Finds a substring"},
    {"strrchr", Group::kString, Flag::kNone, "Finds the last occurrence of a character"},
    {"strtok", Group::kString, Flag::kNone, "Splits a string into tokens"},
    {"_stricmp", Group::kString, Flag::kNone, "Compares 2 strings ignoring case"},
    {"strcpy_s", Group::kString, Flag::kNone, "Copies a string with a size check"},
    {"lstrlenA", Group::kString, Flag::kNone, "Returns the length of a string"},
    {"lstrcpyA", Group::kString, Flag::kNone, "Copies a string"},
    {"lstrcpynA", Group::kString, Flag::kNone, "Copies a string up to a length"},
    {"lstrcatA", Group::kString, Flag::kNone, "Appends a string"},
    {"lstrcmpiA", Group::kString, Flag::kNone, "Compares 2 strings ignoring case"},
    {"wcslen", Group::kString, Flag::kNone, "Returns the length of a UTF-16 string"},
    {"wcscmp", Group::kString, Flag::kNone, "Compares 2 UTF-16 strings"},
    {"wcsncmp", Group::kString, Flag::kNone, "Compares 2 UTF-16 strings up to a length"},
    {"wcscoll", Group::kString, Flag::kNone, "Compares 2 UTF-16 strings by collation"},
    {"wcschr", Group::kString, Flag::kNone, "Finds a character in a UTF-16 string"},
    {"wcsrchr", Group::kString, Flag::kNone, "Finds the last occurrence in a UTF-16 string"},
    {"wcscpy", Group::kString, Flag::kNone, "Copies a UTF-16 string"},
    {"wcsncpy", Group::kString, Flag::kNone, "Copies a UTF-16 string up to a length"},
    {"wcsncpy_s", Group::kString, Flag::kNone, "Copies a UTF-16 string with a size check"},
    {"wcsstr", Group::kString, Flag::kNone, "Finds a UTF-16 substring"},
    {"atan2", Group::kMath, Flag::kNone, "Returns the arc tangent of y over x"},
    {"atof", Group::kMath, Flag::kNone,
     "Parses a floating point number, independent of the locale"},
    {"DebugPrint", Group::kDebug, Flag::kNone,
     "Logs a formatted debug message at Debug level, Shift-JIS converted to UTF-8"},
    {"ConvertThreadToFiber", Group::kThreading, Flag::kNone,
     "Turns the calling thread into a fiber"},
    {"ConvertFiberToThread", Group::kThreading, Flag::kNone,
     "Turns the calling fiber back into a thread"},
    {"CreateFiber", Group::kThreading, Flag::kNone, "Creates a fiber"},
    {"DeleteFiber", Group::kThreading, Flag::kNone, "Deletes a fiber"},
    {"SwitchToFiber", Group::kThreading, Flag::kNone, "Switches to another fiber"},
}};

/**
 * Finds an entry by name.
 *
 * @param name The name without the hook prefix.
 * @return The entry, or null when the name is not registered.
 */
constexpr const Entry* Find(std::string_view name) {
  for (const Entry& entry : kRegistry) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

/**
 * Reports whether a name is registered.
 *
 * @param name The name without the hook prefix.
 * @return True when the registry holds the name.
 */
constexpr bool IsRegistered(std::string_view name) {
  return Find(name) != nullptr;
}

/**
 * Returns the properties of a group.
 *
 * @param group The group.
 * @return The group traits.
 */
constexpr const GroupTraits& TraitsOf(Group group) {
  for (const GroupTraits& traits : kGroups) {
    if (traits.id == group) {
      return traits;
    }
  }
  return kGroups[0];
}

namespace detail {

constexpr bool HasDuplicateNames() {
  for (size_t i = 0; i < kRegistry.size(); ++i) {
    for (size_t j = i + 1; j < kRegistry.size(); ++j) {
      if (kRegistry[i].name == kRegistry[j].name) {
        return true;
      }
    }
  }
  return false;
}

constexpr bool AtomicEntriesRequireHeap() {
  for (const Entry& entry : kRegistry) {
    if (TraitsOf(entry.group).atomic && !HasFlags(entry.flags, Flag::kRequiresHeap)) {
      return false;
    }
  }
  return true;
}

constexpr bool IsEveryGroupListed() {
  for (const Entry& entry : kRegistry) {
    if (TraitsOf(entry.group).id != entry.group) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

static_assert(!detail::HasDuplicateNames(), "shim registry holds a duplicate name");
static_assert(detail::AtomicEntriesRequireHeap(), "an atomic group entry lacks RequiresHeap");
static_assert(detail::IsEveryGroupListed(), "a shim entry names a group missing from kGroups");

}  // namespace rex::kernel::shim
