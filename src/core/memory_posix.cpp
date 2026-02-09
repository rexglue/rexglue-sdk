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

// TODO: Consider splitting this file into multiple files for better organization.
// memory_posix.cpp - core apis for memory management on POSIX systems.
// memory_posix_mapped.cpp - mapped memory implementation on POSIX systems.
// posible split of Linux and Android specific code into their own files.

#include <rex/memory/utils.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_LINUX
#include <fstream>
#include <string>
#include <vector>
#endif

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>

//TODO #include <xenia/base/main_android.h>
#endif

namespace rex {
namespace memory {

namespace {

static inline int PosixFixedFlagsFor(void* base_address) {
  if (!base_address) return 0;
#if defined(MAP_FIXED_NOREPLACE)
  return MAP_FIXED_NOREPLACE;
#else
  return MAP_FIXED;
#endif
}

static inline bool CheckedAdd(uintptr_t a, uintptr_t b, uintptr_t& out) {
#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow)
  return !__builtin_add_overflow(a, b, &out);
#  endif
#endif
  out = a + b;
  return out >= a;
}

}  // namespace

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
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
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

size_t page_size() { return static_cast<size_t>(getpagesize()); }
size_t allocation_granularity() { return page_size(); }

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

bool IsWritableExecutableMemorySupported() { return true; }

#if REX_PLATFORM_LINUX
namespace {

// ============================================================================
// Linux-only helpers
// ============================================================================

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t offset = 0;
  char perms[5] = {};  // "rw-p"
  uint32_t dev_major = 0;
  uint32_t dev_minor = 0;
  uint64_t inode = 0;
  std::string pathname;
};

static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};

  unsigned long long start = 0, end = 0, offset = 0;
  unsigned int maj = 0, min = 0;
  unsigned long inode = 0;
  char perms[5] = {};
  int nconsumed = 0;

  const int matched =
      std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %lu %n", &start, &end,
                  perms, &offset, &maj, &min, &inode, &nconsumed);
  if (matched < 7) return false;

  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  out.offset = static_cast<uintptr_t>(offset);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  out.dev_major = static_cast<uint32_t>(maj);
  out.dev_minor = static_cast<uint32_t>(min);
  out.inode = static_cast<uint64_t>(inode);

  if (nconsumed > 0 && static_cast<size_t>(nconsumed) < line.size()) {
    size_t pos = static_cast<size_t>(nconsumed);
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (pos < line.size()) out.pathname = line.substr(pos);
  }

  return out.start < out.end;
}

template <typename Fn>
static bool ForEachProcMapsEntry(Fn&& fn) {
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open()) return false;

  std::string line;
  LinuxMapEntry e;
  while (std::getline(maps, line)) {
    if (!ParseProcMapsLine(line, e)) continue;
    if (!fn(e)) break;  // false => stop early
  }
  return true;
}

static bool ReadProcMaps(std::vector<LinuxMapEntry>& out_entries) {
  out_entries.clear();
  if (!ForEachProcMapsEntry([&](const LinuxMapEntry& e) {
        out_entries.push_back(e);
        return true;
      })) {
    return false;
  }
  return !out_entries.empty();
}

static bool SameBackingIgnoringPerms(const LinuxMapEntry& a,
                                     const LinuxMapEntry& b) {
  const bool a_private = (a.perms[3] == 'p');
  const bool b_private = (b.perms[3] == 'p');

  // Pathnames can be missing; only compare if both present.
  const bool both_have_path = !a.pathname.empty() && !b.pathname.empty();
  const bool path_ok = !both_have_path || (a.pathname == b.pathname);

  return a_private == b_private && a.dev_major == b.dev_major &&
         a.dev_minor == b.dev_minor && a.inode == b.inode &&
         a.offset == b.offset && path_ok;
}

static bool FindEntryForAddressLinux(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  bool found = false;

  const bool ok = ForEachProcMapsEntry([&](const LinuxMapEntry& e) {
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      found = true;
      return false;
    }
    return true;
  });

  return ok && found;
}

static bool GetContiguousSpanLinux(void* address, uintptr_t& out_start,
                                   uintptr_t& out_end) {
  out_start = 0;
  out_end = 0;

  std::vector<LinuxMapEntry> entries;
  if (!ReadProcMaps(entries)) return false;

  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);

  int idx = -1;
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    if (addr >= entries[i].start && addr < entries[i].end) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return false;

  const LinuxMapEntry& key = entries[idx];
  uintptr_t start = key.start;
  uintptr_t end = key.end;

  for (int i = idx - 1; i >= 0; --i) {
    if (entries[i].end != start) break;
    if (!SameBackingIgnoringPerms(entries[i], key)) break;
    start = entries[i].start;
  }

  for (int i = idx + 1; i < static_cast<int>(entries.size()); ++i) {
    if (entries[i].start != end) break;
    if (!SameBackingIgnoringPerms(entries[i], key)) break;
    end = entries[i].end;
  }

  out_start = start;
  out_end = end;
  return out_start < out_end;
}

static bool IsRangeFullyMappedLinux(void* base_address, size_t length) {
  if (!base_address || length == 0) return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  uintptr_t end = 0;
  if (!CheckedAdd(begin, static_cast<uintptr_t>(length), end)) return false;

  // Ensure [begin,end) has no gaps.
  uintptr_t cursor = begin;
  bool had_any = false;

  if (!ForEachProcMapsEntry([&](const LinuxMapEntry& e) {
        if (e.end <= cursor) return true;
        had_any = true;
        if (e.start > cursor) return false;  // gap
        cursor = e.end;
        return cursor < end;
      })) {
    return false;
  }

  return had_any && cursor >= end;
}

// Arena guard (Linux-only)
static inline uintptr_t HighestPow2LE(uintptr_t x) {
  if (!x) return 0;
  const unsigned int msb =
      63u - static_cast<unsigned int>(
                __builtin_clzll(static_cast<unsigned long long>(x)));
  return uintptr_t(1) << msb;
}

static bool IsRexManagedArenaAddressLinux(void* base_address, size_t length) {
  if (!base_address || length == 0) return false;

  constexpr uintptr_t kMinMappingBase = 0x100000000ULL;
  constexpr uintptr_t kTotalSpan = 0x120000000ULL;

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  if (addr < kMinMappingBase) return false;

  uintptr_t end = 0;
  if (!CheckedAdd(addr, static_cast<uintptr_t>(length), end)) return false;

  const uintptr_t mapping_base = HighestPow2LE(addr);
  if (mapping_base < kMinMappingBase) return false;

  uintptr_t span_end = 0;
  if (!CheckedAdd(mapping_base, kTotalSpan, span_end)) return false;

  return addr >= mapping_base && end <= span_end;
}

static inline void* SanitizeFixedBaseLinux(void* base_address, size_t length) {
  return (base_address && !IsRexManagedArenaAddressLinux(base_address, length))
             ? nullptr
             : base_address;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  const uint32_t prot_requested = ToPosixProtectFlags(access);

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

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  flags |= PosixFixedFlagsFor(base_address);

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }

#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Emulate commit-on-reserve when MAP_FIXED_NOREPLACE hits EEXIST:
  // 1) If already mapped with no gaps, just mprotect.
  // 2) Else, only "repair" via MAP_FIXED if inside our arena.
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    if (IsRangeFullyMappedLinux(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
      return nullptr;
    }

    if (IsRexManagedArenaAddressLinux(base_address, length)) {
      const int repair_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
      void* repaired =
          mmap(base_address, length, static_cast<int>(prot_requested),
               repair_flags, -1, 0);
      return repaired == MAP_FAILED ? nullptr : repaired;
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }

    case DeallocationType::kRelease: {
      size_t unmap_len = length;

#if REX_PLATFORM_LINUX
      if (unmap_len == 0) {
        uintptr_t span_start = 0, span_end = 0;
        if (!GetContiguousSpanLinux(base_address, span_start, span_end)) {
          return false;
        }
        if (span_start != reinterpret_cast<uintptr_t>(base_address)) {
          return false;
        }
        unmap_len = static_cast<size_t>(span_end - span_start);
      }
#else
      if (unmap_len == 0) {
        return false;
      }
#endif

      return munmap(base_address, unmap_len) == 0;
    }

    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (!base_address || length == 0) {
    if (out_old_access) *out_old_access = PageAccess::kNoAccess;
    return false;
  }

  if (out_old_access) {
    size_t ignored_len = 0;
    PageAccess old_access = PageAccess::kNoAccess;
    if (!QueryProtect(base_address, ignored_len, old_access)) {
      *out_old_access = PageAccess::kNoAccess;
      return false;
    }
    *out_old_access = old_access;
  }

  const uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, static_cast<int>(prot)) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;

  LinuxMapEntry e;
  if (!FindEntryForAddressLinux(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);

  const bool r = e.perms[0] == 'r';
  const bool w = e.perms[1] == 'w';
  const bool x = e.perms[2] == 'x';

  if (!r && !w && !x) {
    access_out = PageAccess::kNoAccess;
  } else if (x) {
    access_out = w ? PageAccess::kExecuteReadWrite
                   : PageAccess::kExecuteReadOnly;
  } else {
    access_out = w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
  }

  return true;

#elif REX_PLATFORM_ANDROID
  (void)base_address;
  length = 0;
  access_out = PageAccess::kNoAccess;
  return false;

#elif REX_PLATFORM_MACOS
  (void)base_address;
  length = 0;
  access_out = PageAccess::kNoAccess;
  return false;

#else
#  error "Platform not supported."
#endif
}

namespace {

#if REX_PLATFORM_LINUX || REX_PLATFORM_MACOS
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/') c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}
#endif

}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
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
  return ashmem_fd;

#elif REX_PLATFORM_LINUX || REX_PLATFORM_MACOS

  int oflag;
  switch (access) {
    case PageAccess::kNoAccess: oflag = 0; break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly: oflag = O_RDONLY; break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite: oflag = O_RDWR; break;
    default: assert_always(); return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;

  const std::string shm_name = MakeShmName(path);
  const int fd = shm_open(shm_name.c_str(), oflag, 0777);
  if (fd < 0) return kFileMappingHandleInvalid;

  if (ftruncate(fd, static_cast<off_t>(length)) != 0) {
    close(fd);
    shm_unlink(shm_name.c_str());
    return kFileMappingHandleInvalid;
  }
  return fd;

#else
#  error "Platform not supported."
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);

#if REX_PLATFORM_LINUX || REX_PLATFORM_MACOS
  const std::string shm_name = MakeShmName(path);
  shm_unlink(shm_name.c_str());
#elif REX_PLATFORM_ANDROID
  (void)path;
#else
#  error "Platform not supported."
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  // POSIX requires mmap offset to be page-aligned.
  const size_t page = static_cast<size_t>(getpagesize());
  if (file_offset % page != 0) {
    return nullptr;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_SHARED;

#if REX_PLATFORM_LINUX
  // If fixed was requested, we must either honor it or fail (no silent relocation).
  void* requested = base_address;
  base_address = SanitizeFixedBaseLinux(base_address, length);
  if (requested && !base_address) {
    return nullptr;
  }
#endif

  flags |= PosixFixedFlagsFor(base_address);

  void* result = mmap(base_address, length, static_cast<int>(prot), flags,
                      handle, static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

#if REX_PLATFORM_LINUX
  // Extra safety: if fixed was requested, ensure we actually got it.
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }
#endif

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
