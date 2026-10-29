/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

#include <fcntl.h>

#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // REX_PLATFORM_MAC

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

// macOS off_t is 64-bit with no *64 large-file variants; Linux keeps the
// explicit *64 forms for legacy 32-bit off_t distributions.
#if defined(__APPLE__)
#define rex_mmap64 mmap
#define rex_ftruncate64 ftruncate
#else
#define rex_mmap64 mmap64
#define rex_ftruncate64 ftruncate64
#endif

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

// Convert a filesystem path to a valid shm_open name (must start with /, no other slashes).
// macOS enforces a 31-character total limit, so long names are folded from the full path rather
// than only the filename. This keeps equal filenames in different directories distinct.
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/') {
      c = '_';
    }
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
#if REX_PLATFORM_MAC
  if (name.size() > 30) {
    const std::size_t h = std::hash<std::string>{}(name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    name = hash_buf;
  }
#endif
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
#if REX_PLATFORM_MAC
  // macOS enforces W^X on Apple Silicon. Shared file mappings cannot be both
  // writable and executable. The code cache must use separate RW and RX views.
  return false;
#else
  return true;
#endif
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct MapsRange {
  uintptr_t start = 0;
  uintptr_t end = 0;
  PageAccess access = PageAccess::kNoAccess;
};

using MapsRanges = std::map<uintptr_t, MapsRange>;

struct MapsCache {
  std::mutex mtx;
  MapsRanges ranges;
  std::string scratch;
};

static MapsCache& Cache() {
  static MapsCache* cache = new MapsCache;
  return *cache;
}

constexpr size_t kPermsLen = 3;

static PageAccess RWXToPageAccess(bool r, bool w, bool x) {
  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

static PageAccess PermsToPageAccess(const char perms[kPermsLen]) {
  return RWXToPageAccess(perms[0] == 'r', perms[1] == 'w', perms[2] == 'x');
}

static PageAccess ProtToPageAccess(int prot) {
  return RWXToPageAccess((prot & PROT_READ) != 0, (prot & PROT_WRITE) != 0,
                         (prot & PROT_EXEC) != 0);
}

static void ClearCacheLocked() {
  auto& cache = Cache();
  cache.ranges.clear();
  cache.scratch.clear();
}

static void ReadMaps() {
  ClearCacheLocked();

  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);

  if (fd < 0) {
    return;
  }

  char buffer[8192];
  ssize_t got;

  while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
    Cache().scratch.append(buffer, size_t(got));
  }

  close(fd);

  if (got < 0) {
    return;
  }

  const char* p = Cache().scratch.data();
  const char* end = p + Cache().scratch.size();

  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
    const char* line_end = nl ? nl : end;
    MapsRange range;
    const auto start = std::from_chars(p, line_end, range.start, 16);
    if (start.ec == std::errc{} && start.ptr < line_end && *start.ptr == '-') {
      const auto stop = std::from_chars(start.ptr + 1, line_end, range.end, 16);
      if (stop.ec == std::errc{} && stop.ptr < line_end && *stop.ptr == ' ' &&
          stop.ptr + kPermsLen <= line_end && range.start < range.end) {
        range.access = PermsToPageAccess(stop.ptr + 1);
        Cache().ranges.insert(Cache().ranges.end(), {range.start, range});
      }
    }
    p = line_end + 1;
  }
}

static void EnsureMapsCacheLocked() {
  if (Cache().ranges.empty()) {
    ReadMaps();
  }
}

static MapsRanges::iterator FindLocked(uintptr_t addr) {
  auto& ranges = Cache().ranges;
  const auto it = ranges.upper_bound(addr);

  if (it == ranges.begin()) {
    return ranges.end();
  }

  const auto prev = std::prev(it);
  return prev->second.end > addr ? prev : ranges.end();
}

static MapsRanges::iterator SplitAtLocked(uintptr_t pos) {
  auto& ranges = Cache().ranges;
  const auto it = ranges.upper_bound(pos);

  if (it == ranges.begin()) {
    return it;
  }

  const auto prev = std::prev(it);

  if (prev->second.end <= pos) {
    return it;
  }

  if (prev->first == pos) {
    return prev;
  }

  const MapsRange right{pos, prev->second.end, prev->second.access};
  prev->second.end = pos;
  return ranges.insert(it, {pos, right});
}

static void MergeLocked(MapsRanges::iterator it) {
  auto& ranges = Cache().ranges;

  if (it == ranges.end()) {
    return;
  }

  if (it != ranges.begin()) {
    const auto prev = std::prev(it);
    if (prev->second.end == it->second.start && prev->second.access == it->second.access) {
      prev->second.end = it->second.end;
      ranges.erase(it);
      it = prev;
    }
  }

  auto next = std::next(it);
  while (next != ranges.end() && next->first == it->second.end &&
         next->second.access == it->second.access) {
    it->second.end = next->second.end;
    ranges.erase(next);
    next = std::next(it);
  }
}

static void ApplyAccessLocked(uintptr_t start, uintptr_t end, PageAccess access) {
  auto& ranges = Cache().ranges;

  if (ranges.empty()) {
    ranges[start] = {start, end, access};
    return;
  }

  const auto after = SplitAtLocked(end);
  auto earliest = SplitAtLocked(start);
  auto pos = earliest;

  uintptr_t cursor = start;
  while (cursor < end) {
    if (pos == ranges.end()) {
      const auto ins = ranges.insert(pos, {cursor, {cursor, end, access}});

      if (earliest == pos) {
        earliest = ins;
      }

      break;
    }
    if (pos == after || pos->first > cursor) {
      const uintptr_t hole_end = (pos == after || pos->first > end) ? end : pos->first;
      const auto ins = ranges.insert(pos, {cursor, {cursor, hole_end, access}});

      if (earliest == pos) {
        earliest = ins;
      }

      pos = ins;
      cursor = hole_end;
      ++pos;

      continue;
    }

    pos->second.access = access;
    cursor = pos->second.end;
    ++pos;
  }

  MergeLocked(earliest);
}

static bool PageBounds(void* base_address, size_t length, uintptr_t& start, uintptr_t& end) {
  const size_t page = page_size();
  start = reinterpret_cast<uintptr_t>(base_address) & ~(page - 1);

  if (length > SIZE_MAX - (page - 1)) {
    return false;
  }

  const size_t rounded = (length + page - 1) & ~(page - 1);
  end = start + rounded;

  return end >= start;
}

static void ApplyAccess(void* base_address, size_t length, PageAccess access) {
  uintptr_t start, end;
  std::lock_guard<std::mutex> lock(Cache().mtx);

  if (!PageBounds(base_address, length, start, end)) {
    ClearCacheLocked();
    return;
  }

  ApplyAccessLocked(start, end, access);
}

static void RemoveRange(void* base_address, size_t length) {
  uintptr_t start, end;
  std::lock_guard<std::mutex> lock(Cache().mtx);

  if (!PageBounds(base_address, length, start, end)) {
    ClearCacheLocked();
    return;
  }

  if (Cache().ranges.empty()) {
    return;
  }

  const auto first = SplitAtLocked(start);
  const auto after = SplitAtLocked(end);

  Cache().ranges.erase(first, after);
}

static void InvalidateCache() {
  std::lock_guard<std::mutex> lock(Cache().mtx);
  ClearCacheLocked();
}

static bool LookupMapsProtect(uintptr_t addr, size_t& length, PageAccess& access_out) {
  auto& cache = Cache();
  std::lock_guard<std::mutex> lock(cache.mtx);
  auto it = FindLocked(addr);

  if (it == cache.ranges.end()) {
    ReadMaps();
    it = FindLocked(addr);
    if (it == cache.ranges.end()) {
      length = 0;
      access_out = PageAccess::kNoAccess;
      return false;
    }
  }

  length = size_t(it->second.end - addr);
  access_out = it->second.access;
  return true;
}

static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;

  if (end < begin) {
    return false;
  }

  auto& cache = Cache();
  std::lock_guard<std::mutex> lock(cache.mtx);
  EnsureMapsCacheLocked();

  auto p = FindLocked(begin);
  uintptr_t cursor = begin;

  while (cursor < end) {
    if (p == cache.ranges.end() || p->first > cursor) {
      return false;
    }
    cursor = p->second.end;
    ++p;
  }

  return true;
}

}  // namespace
#else
namespace {
inline void ApplyAccess(void* /*base*/, size_t /*len*/, PageAccess /*access*/) {}
inline void RemoveRange(void* /*base*/, size_t /*len*/) {}
inline void InvalidateCache() {}
inline PageAccess ProtToPageAccess(int /*prot*/) {
  return PageAccess::kNoAccess;
}
inline bool LookupMapsProtect(uintptr_t /*addr*/, size_t& /*length*/, PageAccess& /*out*/) {
  return false;
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

    // On macOS, MAP_FIXED_NOREPLACE is unavailable. kCommit on a pre-reserved
    // range uses mprotect to avoid clobbering the existing reservation. Do not
    // widen sub-host-page requests here - higher-level guest heaps must reconcile
    // all guest permissions sharing a host page first.
#if REX_PLATFORM_MAC
  if (base_address != nullptr && allocation_type == AllocationType::kCommit) {
    const size_t host_page = page_size();
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
    if ((address % host_page) != 0 || (length % host_page) != 0) {
      return nullptr;
    }
    if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
      return base_address;
    }
    return nullptr;
  }
#endif

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if REX_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite || access == PageAccess::kExecuteReadOnly) {
    flags |= MAP_JIT;
  }
  if (base_address) {
    flags |= MAP_FIXED;
  }
#elif defined(MAP_FIXED_NOREPLACE)
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
    ApplyAccess(result, length, ProtToPageAccess(prot_initial));
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
        ApplyAccess(base_address, length, access);
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
      // Decommit: remove access first, then release physical pages
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        InvalidateCache();
        return false;
      }
      ApplyAccess(base_address, length, PageAccess::kNoAccess);
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      size_t release_length = length;
      if (release_length == 0) {
        size_t region = 0;
        PageAccess region_access;
        if (!LookupMapsProtect(reinterpret_cast<uintptr_t>(base_address), region, region_access)) {
          return false;
        }
        release_length = region;
      }
      const bool ok = munmap(base_address, release_length) == 0;
      if (ok) {
        RemoveRange(base_address, release_length);
      } else {
        InvalidateCache();
      }
      return ok;
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

#if REX_PLATFORM_MAC
  // mprotect doesn't report the previous protection. Query the Mach region
  // before changing it so this matches VirtualProtect's out parameter.
  if (out_old_access) {
    size_t old_region_length = 0;
    QueryProtect(base_address, old_region_length, *out_old_access);
  }
#elif REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    size_t old_region_length = 0;
    LookupMapsProtect(reinterpret_cast<uintptr_t>(base_address), old_region_length,
                      *out_old_access);
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    REXSYS_ERROR("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length, prot,
                 strerror(errno), errno);
    InvalidateCache();
  } else {
    ApplyAccess(base_address, length, access);
  }
  return ret == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr =
      mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
  if (kr != KERN_SUCCESS) {
    return false;
  }
  if (address > reinterpret_cast<mach_vm_address_t>(base_address)) {
    return false;
  }

  length = static_cast<size_t>((address + region_size) -
                               reinterpret_cast<mach_vm_address_t>(base_address));

  if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
      (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) ==
             (VM_PROT_READ | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE)) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }
  return true;
#elif !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  return LookupMapsProtect(reinterpret_cast<uintptr_t>(base_address), length, access_out);
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
  if (rex_ftruncate64(ret, static_cast<off_t>(length)) != 0) {
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

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = rex_mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                            static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    if (munmap(result, length) == 0) {
      RemoveRange(result, length);
    } else {
      InvalidateCache();
    }
    return nullptr;
  }

  ApplyAccess(result, length, access);
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  const bool ok = munmap(base_address, length) == 0;
  if (ok) {
    RemoveRange(base_address, length);
  } else {
    InvalidateCache();
  }
  return ok;
}

}  // namespace memory
}  // namespace rex
