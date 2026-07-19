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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging.h>
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
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

// ---------------------------------------------------------------------------
// Host protection shadow. See EnableProtectShadow() in rex/memory/utils.h for
// why this exists. Lock-free by construction: the table is allocated once and
// never resized, and each page is a single relaxed atomic byte, so it is safe
// to read from the SIGSEGV handler.
//
// Encoding: 0 == unknown (never recorded -> caller falls back to the
// /proc/self/maps path), otherwise 1 + uint8_t(PageAccess).
// ---------------------------------------------------------------------------
namespace {

std::atomic<uint8_t>* g_shadow_pages = nullptr;
uintptr_t g_shadow_base = 0;
size_t g_shadow_page_count = 0;

constexpr uint8_t kShadowUnknown = 0;

inline bool ShadowPageRange(void* address, size_t length, size_t& first, size_t& count) {
  if (!g_shadow_pages || length == 0) {
    return false;
  }
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  if (addr < g_shadow_base) {
    return false;
  }
  const size_t ps = page_size();
  const size_t offset = addr - g_shadow_base;
  first = offset / ps;
  if (first >= g_shadow_page_count) {
    return false;
  }
  // Round the end up so a partial page is fully covered.
  const size_t end_page = (offset + length + ps - 1) / ps;
  count = std::min(end_page, g_shadow_page_count) - first;
  return count != 0;
}

// Publish `access` for the range. Called *before* mprotect so that tightening
// protection is never briefly stale-loose (a stale-loose read is what makes the
// fault handler wrongly conclude "another thread cleared this watch").
void ShadowRecord(void* address, size_t length, PageAccess access) {
  size_t first = 0, count = 0;
  if (!ShadowPageRange(address, length, first, count)) {
    return;
  }
  const uint8_t value = static_cast<uint8_t>(static_cast<uint8_t>(access) + 1);
  for (size_t i = 0; i < count; ++i) {
    g_shadow_pages[first + i].store(value, std::memory_order_relaxed);
  }
}

// Drop back to "unknown" so the range is answered by the slow path again. Used
// when a protection change fails or the mapping goes away entirely.
void ShadowInvalidate(void* address, size_t length) {
  size_t first = 0, count = 0;
  if (!ShadowPageRange(address, length, first, count)) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    g_shadow_pages[first + i].store(kShadowUnknown, std::memory_order_relaxed);
  }
}

bool ShadowLookup(void* address, PageAccess& access_out) {
  size_t first = 0, count = 0;
  if (!ShadowPageRange(address, 1, first, count)) {
    return false;
  }
  const uint8_t value = g_shadow_pages[first].load(std::memory_order_relaxed);
  if (value == kShadowUnknown) {
    return false;
  }
  access_out = static_cast<PageAccess>(value - 1);
  return true;
}

}  // namespace

void EnableProtectShadow(void* base_address, size_t length) {
  if (g_shadow_pages) {
    return;  // idempotent
  }
  const size_t ps = page_size();
  const size_t page_count = (length + ps - 1) / ps;
  if (page_count == 0) {
    return;
  }
  auto* pages = new (std::nothrow) std::atomic<uint8_t>[page_count];
  if (!pages) {
    REXLOG_WARN("EnableProtectShadow: allocation failed ({} pages); "
                "QueryProtect stays on the /proc/self/maps path",
                page_count);
    return;
  }
  for (size_t i = 0; i < page_count; ++i) {
    pages[i].store(kShadowUnknown, std::memory_order_relaxed);
  }
  g_shadow_base = reinterpret_cast<uintptr_t>(base_address);
  g_shadow_page_count = page_count;
  // Publish last: readers check g_shadow_pages first.
  g_shadow_pages = pages;
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

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
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

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
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
    ShadowRecord(result, length,
                 allocation_type == AllocationType::kReserve ? PageAccess::kNoAccess : access);
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
      ShadowRecord(base_address, length, access);
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
      ShadowInvalidate(base_address, length);
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
      ShadowRecord(base_address, length, PageAccess::kNoAccess);
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        ShadowInvalidate(base_address, length);
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      // The mapping is gone entirely - any recorded protection is meaningless.
      ShadowInvalidate(base_address, length);
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

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  // Publish before the syscall so tightening is never stale-loose; on failure
  // fall back to "unknown" rather than leaving a wrong entry behind.
  ShadowRecord(base_address, length, access);
  if (mprotect(base_address, length, prot) != 0) {
    ShadowInvalidate(base_address, length);
    return false;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  // Fast path: a page this layer protected itself. Only ever consulted for
  // pages we explicitly recorded, so a miss degrades to the parse below rather
  // than answering wrongly. The sole caller (MMIOHandler's write-watch fault
  // path) ignores `length`, so reporting a single page here is sufficient.
  {
    PageAccess shadow_access;
    if (ShadowLookup(base_address, shadow_access)) {
      access_out = shadow_access;
      length = page_size();
      return true;
    }
  }

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

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
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Unlink the name now and keep only the descriptor. The mapping stays valid
  // for the life of the fd (mmap does not need the name, and nothing reopens
  // this object by name), but the /dev/shm entry is gone immediately, so the
  // kernel reclaims the backing store when the last fd closes -- on any exit
  // path, including SIGKILL and hard crashes.
  //
  // Without this, only a clean shutdown reached CloseFileMappingHandle, so
  // every crash, hang-kill or watchdog SIGKILL leaked a full-size guest memory
  // file (4.5 GB for FH1) into /dev/shm. Since that is tmpfs, the leak is RAM:
  // an iteration session accumulated 36 orphans holding ~11 GB, filled
  // /dev/shm to 100%, degraded the whole desktop, and produced spurious SIGBUS
  // (touching a page tmpfs can no longer back) in unrelated code under test.
  shm_unlink(full_path.c_str());
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  // Normally already unlinked at creation; this is a no-op safety net that also
  // cleans up an object created by an older build that did not unlink early.
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

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                        static_cast<off_t>(file_offset));
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
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
