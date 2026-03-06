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

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

namespace rex {
namespace memory {

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  static const size_t value = []() -> size_t {
    long sysconf_page_size = sysconf(_SC_PAGESIZE);
    if (sysconf_page_size > 0) {
      return static_cast<size_t>(sysconf_page_size);
    }
    return static_cast<size_t>(getpagesize());
  }();
  return value;
}
size_t allocation_granularity() {
  // Mirrors current POSIX behavior where granularity equals page size.
  static const size_t value = page_size();
  return value;
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
  return true;
}

namespace {

static bool AlignRangeToPageBounds(void* base_address, size_t length, void*& aligned_base_address,
                                   size_t& aligned_length) {
  if (!base_address || !length) {
    return false;
  }
  const uintptr_t page = page_size();
  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t start = addr & ~(page - 1);
  const uintptr_t end_unaligned = addr + length;
  if (end_unaligned < addr) {
    return false;
  }
  const uintptr_t end = rex::round_up(end_unaligned, page);
  aligned_base_address = reinterpret_cast<void*>(start);
  aligned_length = static_cast<size_t>(end - start);
  return aligned_length != 0;
}

}  // namespace

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry.
static bool ParseProcMapsLine(const char* line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line, "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

static std::vector<LinuxMapEntry>& GetMapsScratch() {
  thread_local std::vector<LinuxMapEntry> entries;
  return entries;
}

static bool ReadProcMaps(std::vector<LinuxMapEntry>& out_entries) {
  out_entries.clear();
  if (out_entries.capacity() < 2048) {
    out_entries.reserve(2048);
  }

  FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) {
    return false;
  }

  char line[512];
  while (std::fgets(line, sizeof(line), maps)) {
    LinuxMapEntry entry;
    if (ParseProcMapsLine(line, entry)) {
      out_entries.push_back(entry);
    }
  }
  std::fclose(maps);
  return !out_entries.empty();
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  auto& entries = GetMapsScratch();
  if (!ReadProcMaps(entries)) {
    return false;
  }
  uintptr_t cursor = begin;
  for (const LinuxMapEntry& entry : entries) {
    if (entry.end <= cursor) {
      continue;
    }
    if (entry.start > cursor) {
      return false;  // gap found
    }
    cursor = entry.end;
    if (cursor >= end) {
      return true;
    }
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

static bool FindRegionForAddress(void* address, uintptr_t& out_region_start,
                                 uintptr_t& out_region_end, PageAccess& out_access) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  auto& entries = GetMapsScratch();
  if (!ReadProcMaps(entries)) {
    return false;
  }

  size_t found_index = size_t(-1);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (addr >= entries[i].start && addr < entries[i].end) {
      found_index = i;
      break;
    }
  }
  if (found_index == size_t(-1)) {
    return false;
  }

  out_access = PermsToPageAccess(entries[found_index].perms);
  out_region_start = entries[found_index].start;
  out_region_end = entries[found_index].end;

  for (size_t i = found_index; i > 0; --i) {
    const auto& prev = entries[i - 1];
    const auto& cur = entries[i];
    if (prev.end != cur.start || PermsToPageAccess(prev.perms) != out_access) {
      break;
    }
    out_region_start = prev.start;
  }
  for (size_t i = found_index + 1; i < entries.size(); ++i) {
    const auto& prev = entries[i - 1];
    const auto& cur = entries[i];
    if (prev.end != cur.start || PermsToPageAccess(cur.perms) != out_access) {
      break;
    }
    out_region_end = cur.end;
  }

  return true;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

  // Build flags - always use MAP_FIXED_NOREPLACE for fixed addresses
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      void* aligned_base_address = nullptr;
      size_t aligned_length = 0;
      if (!AlignRangeToPageBounds(base_address, length, aligned_base_address, aligned_length)) {
        return false;
      }
      // Decommit: remove access first, then release physical pages
      if (mprotect(aligned_base_address, aligned_length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(aligned_base_address, aligned_length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      return munmap(base_address, length) == 0;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

  void* aligned_base_address = nullptr;
  size_t aligned_length = 0;
  if (!AlignRangeToPageBounds(base_address, length, aligned_base_address, aligned_length)) {
    return false;
  }

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    uintptr_t region_start = 0;
    uintptr_t region_end = 0;
    PageAccess region_access = PageAccess::kNoAccess;
    if (FindRegionForAddress(aligned_base_address, region_start, region_end, region_access)) {
      *out_old_access = region_access;
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(aligned_base_address, aligned_length, prot) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  uintptr_t region_start = 0;
  uintptr_t region_end = 0;
  if (!FindRegionForAddress(base_address, region_start, region_end, access_out)) {
    return false;
  }

  length = static_cast<size_t>(region_end - region_start);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#else
  (void)access;
  (void)commit;
  int oflag = O_CREAT | O_RDWR;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  // Windows rounds mapping object size to page granularity internally.
  // Do the same on POSIX to preserve cross-platform map range behavior.
  const size_t aligned_length = rex::round_up(length, page_size());
  if (aligned_length < length || ftruncate64(ret, static_cast<off_t>(aligned_length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }
  const int fd = static_cast<int>(handle);
  struct stat64 file_stat;
  if (fstat64(fd, &file_stat) != 0) {
    return nullptr;
  }
  const uint64_t mapped_size = uint64_t(rex::round_up(size_t(file_stat.st_size), page));
  const uint64_t mapping_end = uint64_t(file_offset) + uint64_t(length);
  if (mapping_end < file_offset || mapping_end > mapped_size) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = mmap64(base_address, length, prot, flags, fd, static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
