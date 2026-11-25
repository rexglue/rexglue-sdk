#pragma once
/**
 * Profile-guided optimization support (REXGLUE_PGO=GENERATE builds, Linux).
 *
 * Each instrumented shared library carries its own copy of the clang profile
 * runtime, with hidden symbols, so WriteModuleProfile writes the counters of
 * the library whose code calls it. Writing periodically means the data
 * survives the game being killed rather than exited (Steam, a crash).
 */

#if defined(__linux__)

#include <unistd.h>

#include <string>

extern "C" {
int __llvm_profile_write_file(void) __attribute__((weak));
void __llvm_profile_reset_counters(void) __attribute__((weak));
void __llvm_profile_set_filename(const char*) __attribute__((weak));
}

namespace rex::pgo {

// Merges this library's counters since the last call into
// rex-pgo-<module signature>.profraw next to the executable, then resets them
// (so the next call adds only new counts). Returns false when the library is
// not instrumented.
static inline bool WriteModuleProfile() {
  if (!__llvm_profile_write_file || !__llvm_profile_reset_counters) {
    return false;
  }
  static std::string filename;
  if (filename.empty() && __llvm_profile_set_filename) {
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string dir;
    if (n > 0) {
      exe[n] = 0;
      dir.assign(exe);
      dir.resize(dir.rfind('/') + 1);
    }
    // %m: one file per instrumented library, merged into on each write.
    filename = dir + "rex-pgo-%m.profraw";
    __llvm_profile_set_filename(filename.c_str());
  }
  if (__llvm_profile_write_file() == 0) {
    __llvm_profile_reset_counters();
  }
  return true;
}

}  // namespace rex::pgo

#endif  // __linux__
