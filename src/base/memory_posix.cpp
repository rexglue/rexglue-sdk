/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <charconv>
#include <format>
#include <random>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <signal.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform.h>

#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // REX_PLATFORM_MAC

#include "base/memory_posix.h"

#if REX_PLATFORM_LINUX
#include "base/proc_map.h"
#endif

static_assert(sizeof(off_t) == 8);

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

#endif

namespace rex {
namespace memory {

/** Builds a shm_open name from a path, hashing long names on macOS (31 character limit). */
static std::string BuildSHMName(const std::filesystem::path& path) {
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
static void* libandroid_;
// Android API 26 and later.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidAPILevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    REX_ASSERT(libandroid_ != nullptr);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      REX_ASSERT(android_ASharedMemory_create_ != nullptr);
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

size_t GetPageSize() {
  return getpagesize();
}
size_t GetAllocationGranularity() {
  return GetPageSize();
}

int ToPosixProtectFlags(PageAccess access) {
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
      REX_UNHANDLED_CASE(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
#if REX_PLATFORM_MAC
  // macOS enforces W^X on Apple Silicon, so the code cache needs separate RW and RX views.
  return false;
#else
  return true;
#endif
}

#if REX_PLATFORM_LINUX
namespace {

bool IsRangeFullyMapped(void* base_address, size_t length) {
  return base_address && length != 0 && msync(base_address, length, MS_ASYNC) == 0;
}

PageAccess PermsToPageAccess(const std::array<char, 4>& perms) {
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

/** Emulates VirtualAlloc. Reserve maps PROT_NONE, and commit on a reservation uses mprotect. */
void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  const int prot_requested = ToPosixProtectFlags(access);

  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = prot_requested;
      break;
  }

#if REX_PLATFORM_MAC
  // Commits not aligned to host pages fail, since guest heaps reconcile permissions inside a page.
  if (base_address != nullptr && allocation_type == AllocationType::kCommit) {
    const size_t host_page = GetPageSize();
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
    if ((address % host_page) != 0 || (length % host_page) != 0) {
      return nullptr;
    }
    if (mprotect(base_address, length, prot_requested) == 0) {
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

#if REX_PLATFORM_LINUX
  if (allocation_type != AllocationType::kReserve && IsRangeFullyMapped(base_address, length)) {
    return mprotect(base_address, length, prot_requested) == 0 ? base_address : nullptr;
  }
#endif

  void* result = detail::AcceptFixedMapping(base_address, length,
                                            mmap(base_address, length, prot_initial, flags, -1, 0));
  return result != MAP_FAILED ? result : nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
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
      return munmap(base_address, length) == 0;
    }
    default:
      REX_ASSERT_ALWAYS();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_MAC
  // mprotect reports no previous protection, so the Mach region is queried first.
  if (out_old_access) {
    size_t old_region_length = 0;
    QueryProtect(base_address, old_region_length, *out_old_access);
  }
#elif REX_PLATFORM_LINUX
  // Query and mprotect are not atomic, so another thread can change the protection between them.
  if (out_old_access) {
    ProcMapping e;
    if (FindMapping(reinterpret_cast<u64>(base_address), e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  const int prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    REXLOG_ERROR(system, "mprotect({}, 0x{:X}, {}) failed with {} ({})", base_address, length, prot,
                 strerror(errno), errno);
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

  ProcMapping e;
  if (!FindMapping(reinterpret_cast<u64>(base_address), e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, [[maybe_unused]] bool commit) {
#if REX_PLATFORM_ANDROID
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // /dev/ashmem replaces ASharedMemory below API 26 and is disabled for apps targeting API 29.
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::CountOf(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      REX_ASSERT_ALWAYS();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT | O_EXCL;
  auto full_path = BuildSHMName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return ret;
#endif
}

std::filesystem::path CreateFileMappingName(std::string_view prefix) {
  std::random_device random;
  return std::format("{}{}_{:08x}{:08x}", prefix, static_cast<int>(getpid()), random(), random());
}

void RemoveStaleFileMappings([[maybe_unused]] std::string_view prefix) {
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
  std::error_code error;
  std::filesystem::directory_iterator it("/dev/shm", error);
  for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
    const std::string name = it->path().filename().string();
    if (!name.starts_with(prefix)) {
      continue;
    }
    const char* digits = name.data() + prefix.size();
    const char* end = name.data() + name.size();
    int process_id = 0;
    const auto parsed = std::from_chars(digits, end, process_id);
    if (parsed.ec != std::errc() || parsed.ptr == digits || parsed.ptr == end ||
        *parsed.ptr != '_') {
      continue;
    }
    if (kill(process_id, 0) == -1 && errno == ESRCH) {
      shm_unlink(("/" + name).c_str());
    }
  }
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(handle);
#if !REX_PLATFORM_ANDROID
  auto full_path = BuildSHMName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  const size_t page = GetPageSize();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // MAP_FIXED replaces the reserved range, MAP_FIXED_NOREPLACE would fail with EEXIST.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  const int prot = ToPosixProtectFlags(access);
  void* result = mmap(base_address, length, prot, flags, handle, static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView([[maybe_unused]] FileMappingHandle handle, void* base_address, size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
