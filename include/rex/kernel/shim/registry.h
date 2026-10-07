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
  File,      /**< File I/O backed by the filesystem module. */
  Heap,      /**< The o1heap backed heap, all 4 entries are required together. */
  Memory,    /**< Memory copy and fill routines. */
  String,    /**< C, Win32 and UTF-16 string routines. */
  Threading, /**< Fiber routines. */
};

/** Behavior flags of a shim entry. */
enum class Flag : u8 {
  None = 0,
  Stub = 1 << 0,         /**< Returns success without doing the work. */
  RequiresHeap = 1 << 1, /**< Needs the shim heap initialized. */
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

inline constexpr std::array<GroupTraits, 5> kGroups = {{
    {Group::File, "file", false, "File I/O backed by the filesystem module"},
    {Group::Heap, "heap", true, "The o1heap backed heap, all 4 routines are required together"},
    {Group::Memory, "memory", false, "Memory copy and fill routines"},
    {Group::String, "string", false, "C, Win32 and UTF-16 string routines"},
    {Group::Threading, "threading", false, "Fiber routines"},
}};

inline constexpr std::array<Entry, 66> kRegistry = {{
    {"CreateFileA", Group::File, Flag::None, "Opens or creates a file"},
    {"ReadFile", Group::File, Flag::None, "Reads from an open file handle"},
    {"WriteFile", Group::File, Flag::None, "Writes to an open file handle"},
    {"SetFilePointer", Group::File, Flag::None, "Moves the file position with a 32 bit distance"},
    {"GetFileSize", Group::File, Flag::None, "Returns the low 32 bits of a file size"},
    {"GetFileSizeEx", Group::File, Flag::None, "Returns the 64 bit file size"},
    {"SetEndOfFile", Group::File, Flag::None, "Truncates a file at the current position"},
    {"FlushFileBuffers", Group::File, Flag::None, "Flushes a file"},
    {"DeleteFileA", Group::File, Flag::None, "Deletes a file"},
    {"CloseHandle", Group::File, Flag::None, "Closes a file or search handle"},
    {"FindFirstFileA", Group::File, Flag::None, "Begins a directory search"},
    {"FindNextFileA", Group::File, Flag::None, "Returns the next directory search result"},
    {"FindClose", Group::File, Flag::None, "Ends a directory search"},
    {"CreateDirectoryA", Group::File, Flag::None, "Creates a directory"},
    {"MoveFileA", Group::File, Flag::None, "Renames a file or directory"},
    {"SetFileAttributesA", Group::File, Flag::None, "Sets file attributes"},
    {"GetFileAttributesA", Group::File, Flag::None, "Returns file attributes"},
    {"GetFileAttributesExA", Group::File, Flag::None, "Returns extended file attribute data"},
    {"SetFilePointerEx", Group::File, Flag::None, "Moves the file position with a 64 bit distance"},
    {"SetFileTime", Group::File, Flag::None, "Sets the file timestamps"},
    {"CompareFileTime", Group::File, Flag::None, "Compares 2 file times"},
    {"CopyFileA", Group::File, Flag::None, "Copies a file"},
    {"RemoveDirectoryA", Group::File, Flag::None, "Removes an empty directory"},
    {"GetFileType", Group::File, Flag::None, "Returns the type of a file handle"},
    {"RtlAllocateHeap", Group::Heap, Flag::RequiresHeap, "Allocates memory, honors the zero flag"},
    {"RtlFreeHeap", Group::Heap, Flag::RequiresHeap, "Frees an allocation"},
    {"RtlSizeHeap", Group::Heap, Flag::RequiresHeap, "Returns the requested size of an allocation"},
    {"RtlReAllocateHeap", Group::Heap, Flag::RequiresHeap,
     "Resizes an allocation in place or by copy"},
    {"memcpy", Group::Memory, Flag::None, "Copies memory"},
    {"memmove", Group::Memory, Flag::None, "Copies overlapping memory"},
    {"memset", Group::Memory, Flag::None, "Fills memory"},
    {"memchr", Group::Memory, Flag::None, "Finds a byte in memory"},
    {"XMemCpy", Group::Memory, Flag::None, "Copies memory"},
    {"XMemSet", Group::Memory, Flag::None, "Fills memory"},
    {"XMemSet128", Group::Memory, Flag::None, "Fills memory in 128 byte blocks"},
    {"memset_vmx", Group::Memory, Flag::None, "Fills memory through the vector path"},
    {"memcpy_s", Group::Memory, Flag::None, "Copies memory with a size check"},
    {"memmove_s", Group::Memory, Flag::None, "Copies overlapping memory with a size check"},
    {"strncmp", Group::String, Flag::None, "Compares 2 strings up to a length"},
    {"strncpy", Group::String, Flag::None, "Copies a string up to a length"},
    {"strchr", Group::String, Flag::None, "Finds a character in a string"},
    {"strstr", Group::String, Flag::None, "Finds a substring"},
    {"strrchr", Group::String, Flag::None, "Finds the last occurrence of a character"},
    {"strtok", Group::String, Flag::None, "Splits a string into tokens"},
    {"_stricmp", Group::String, Flag::None, "Compares 2 strings ignoring case"},
    {"strcpy_s", Group::String, Flag::None, "Copies a string with a size check"},
    {"lstrlenA", Group::String, Flag::None, "Returns the length of a string"},
    {"lstrcpyA", Group::String, Flag::None, "Copies a string"},
    {"lstrcpynA", Group::String, Flag::None, "Copies a string up to a length"},
    {"lstrcatA", Group::String, Flag::None, "Appends a string"},
    {"lstrcmpiA", Group::String, Flag::None, "Compares 2 strings ignoring case"},
    {"wcslen", Group::String, Flag::None, "Returns the length of a UTF-16 string"},
    {"wcscmp", Group::String, Flag::None, "Compares 2 UTF-16 strings"},
    {"wcsncmp", Group::String, Flag::None, "Compares 2 UTF-16 strings up to a length"},
    {"wcscoll", Group::String, Flag::None, "Compares 2 UTF-16 strings by collation"},
    {"wcschr", Group::String, Flag::None, "Finds a character in a UTF-16 string"},
    {"wcsrchr", Group::String, Flag::None, "Finds the last occurrence in a UTF-16 string"},
    {"wcscpy", Group::String, Flag::None, "Copies a UTF-16 string"},
    {"wcsncpy", Group::String, Flag::None, "Copies a UTF-16 string up to a length"},
    {"wcsncpy_s", Group::String, Flag::None, "Copies a UTF-16 string with a size check"},
    {"wcsstr", Group::String, Flag::None, "Finds a UTF-16 substring"},
    {"ConvertThreadToFiber", Group::Threading, Flag::None, "Turns the calling thread into a fiber"},
    {"ConvertFiberToThread", Group::Threading, Flag::None,
     "Turns the calling fiber back into a thread"},
    {"CreateFiber", Group::Threading, Flag::None, "Creates a fiber"},
    {"DeleteFiber", Group::Threading, Flag::None, "Deletes a fiber"},
    {"SwitchToFiber", Group::Threading, Flag::None, "Switches to another fiber"},
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
 * Tests whether a name is registered.
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
    if (TraitsOf(entry.group).atomic && !HasFlags(entry.flags, Flag::RequiresHeap)) {
      return false;
    }
  }
  return true;
}

constexpr bool EveryGroupListed() {
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
static_assert(detail::EveryGroupListed(), "a shim entry names a group missing from kGroups");

}  // namespace rex::kernel::shim
