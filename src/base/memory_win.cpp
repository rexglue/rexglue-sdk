/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "platform_win.h"

#include <format>
#include <random>
#include <string_view>

#include <rex/logging/log_category.h>
#include <rex/memory/virtual_memory.h>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM | \
                            WINAPI_PARTITION_GAMES)
#define REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif

namespace rex {
namespace memory {

size_t GetPageSize() {
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwPageSize;
  }
  return value;
}

size_t GetAllocationGranularity() {
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwAllocationGranularity;
  }
  return value;
}

DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PAGE_NOACCESS;
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      REX_UNHANDLED_CASE(access);
      return PAGE_NOACCESS;
  }
}

PageAccess ToXeniaProtectFlags(DWORD access) {
  if (access & PAGE_GUARD) {
    access &= ~PAGE_GUARD;
  }

  switch (access) {
    case PAGE_NOACCESS:
      return PageAccess::kNoAccess;
    case PAGE_READONLY:
      return PageAccess::kReadOnly;
    case PAGE_READWRITE:
      return PageAccess::kReadWrite;
    case PAGE_EXECUTE_READ:
      return PageAccess::kExecuteReadOnly;
    case PAGE_EXECUTE_READWRITE:
      return PageAccess::kExecuteReadWrite;
    default:
      return PageAccess::kNoAccess;
  }
}

bool IsWritableExecutableMemorySupported() {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return true;
#else
  return false;
#endif
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
    default:
      REX_UNHANDLED_CASE(allocation_type);
      break;
  }
  DWORD protect = ToWin32ProtectFlags(access);
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return VirtualAlloc(base_address, length, alloc_type, protect);
#else
  return VirtualAllocFromApp(base_address, length, static_cast<ULONG>(alloc_type),
                             static_cast<ULONG>(protect));
#endif
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  DWORD free_type = 0;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      free_type = MEM_RELEASE;
      length = 0;
      break;
    case DeallocationType::kDecommit:
      free_type = MEM_DECOMMIT;
      break;
    default:
      REX_UNHANDLED_CASE(deallocation_type);
      break;
  }
  return VirtualFree(base_address, length, free_type) ? true : false;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  DWORD new_protect = ToWin32ProtectFlags(access);
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD old_protect = 0;
  BOOL result = VirtualProtect(base_address, length, new_protect, &old_protect);
#else
  ULONG old_protect = 0;
  BOOL result =
      VirtualProtectFromApp(base_address, length, static_cast<ULONG>(new_protect), &old_protect);
#endif
  if (!result) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = ToXeniaProtectFlags(old_protect);
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;

  MEMORY_BASIC_INFORMATION info;
  ZeroMemory(&info, sizeof(info));

  SIZE_T result = VirtualQuery(base_address, &info, sizeof(info));
  if (!result) {
    return false;
  }

  length = info.RegionSize;
  access_out = ToXeniaProtectFlags(info.Protect);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  DWORD protect = ToWin32ProtectFlags(access) | (commit ? SEC_COMMIT : SEC_RESERVE);
  auto full_path = "Local" / path;
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  HANDLE h =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect, static_cast<DWORD>(length >> 32),
                         static_cast<DWORD>(length), full_path.c_str());
#else
  HANDLE h = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, static_cast<ULONG>(protect),
                                      static_cast<ULONG64>(length), full_path.c_str());
#endif
  if (!h) {
    return kFileMappingHandleInvalid;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(h);
    return kFileMappingHandleInvalid;
  }
  return h;
}

std::filesystem::path CreateFileMappingName(std::string_view prefix) {
  std::random_device random;
  return std::format("{}{}_{:08x}{:08x}", prefix, GetCurrentProcessId(), random(), random());
}

void RemoveStaleFileMappings([[maybe_unused]] std::string_view prefix) {}

void CloseFileMappingHandle(FileMappingHandle handle,
                            [[maybe_unused]] const std::filesystem::path& path) {
  CloseHandle(handle);
}

namespace {

#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
// VirtualAlloc2 and MapViewOfFile3 need Windows 10 1803. The import libraries of the desktop
// Windows SDK do not provide them, so they are resolved from kernelbase.dll.
using VirtualAlloc2Function = PVOID(WINAPI*)(HANDLE process, PVOID base_address, SIZE_T size,
                                             ULONG allocation_type, ULONG protect,
                                             void* extended_parameters, ULONG parameter_count);
using MapViewOfFile3Function = PVOID(WINAPI*)(HANDLE file_mapping, HANDLE process,
                                              PVOID base_address, ULONG64 offset, SIZE_T view_size,
                                              ULONG allocation_type, ULONG protect,
                                              void* extended_parameters, ULONG parameter_count);

struct PlaceholderFunctions {
  VirtualAlloc2Function alloc = nullptr;
  MapViewOfFile3Function map = nullptr;
};

const PlaceholderFunctions& GetPlaceholderFunctions() {
  static const PlaceholderFunctions functions = [] {
    PlaceholderFunctions resolved;
    if (HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
      resolved.alloc =
          reinterpret_cast<VirtualAlloc2Function>(GetProcAddress(kernelbase, "VirtualAlloc2"));
      resolved.map =
          reinterpret_cast<MapViewOfFile3Function>(GetProcAddress(kernelbase, "MapViewOfFile3"));
    }
    if (!resolved.alloc || !resolved.map) {
      REXSYS_ERROR("MapFileView needs VirtualAlloc2 and MapViewOfFile3 from Windows 10 1803");
      resolved = {};
    }
    return resolved;
  }();
  return functions;
}
#endif

void* ReservePlaceholder(void* base_address, size_t length) {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  const PlaceholderFunctions& functions = GetPlaceholderFunctions();
  if (!functions.alloc) {
    return nullptr;
  }
  return functions.alloc(GetCurrentProcess(), base_address, length,
                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
#else
  // VirtualAlloc2FromApp and MapViewOfFile3FromApp require 10.0.17134.0.
  return VirtualAlloc2FromApp(GetCurrentProcess(), base_address, length,
                              MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
#endif
}

void* ReplacePlaceholder(HANDLE handle, void* placeholder, size_t length, DWORD protect,
                         size_t file_offset) {
#ifdef REX_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return GetPlaceholderFunctions().map(handle, GetCurrentProcess(), placeholder,
                                       static_cast<ULONG64>(file_offset), length,
                                       MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
#else
  return MapViewOfFile3FromApp(handle, GetCurrentProcess(), placeholder,
                               static_cast<ULONG64>(file_offset), length, MEM_REPLACE_PLACEHOLDER,
                               static_cast<ULONG>(protect), nullptr, 0);
#endif
}

}  // namespace

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  if (access == PageAccess::kNoAccess || file_offset % GetPageSize() != 0) {
    return nullptr;
  }
  // A view that replaces a placeholder needs a page aligned file offset, not a multiple of the
  // allocation granularity, so the mapping can start at a 4 KiB offset.
  void* placeholder = ReservePlaceholder(base_address, length);
  if (!placeholder) {
    return nullptr;
  }
  void* mapping =
      ReplacePlaceholder(handle, placeholder, length, ToWin32ProtectFlags(access), file_offset);
  if (!mapping) {
    VirtualFree(placeholder, 0, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
}

bool UnmapFileView([[maybe_unused]] FileMappingHandle handle, void* base_address,
                   [[maybe_unused]] size_t length) {
  return UnmapViewOfFile(base_address) ? true : false;
}

}  // namespace memory
}  // namespace rex
