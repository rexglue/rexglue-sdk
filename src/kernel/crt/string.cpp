/**
 * @file        kernel/crt/string.cpp
 *
 * @brief       Native string operation hooks -- replaces recompiled PPC
 *              implementations of strncmp, strchr, lstrlenA, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/platform.h>

#if !REX_PLATFORM_WIN32
#include <strings.h>
#endif

#include <rex/ppc/function.h>

namespace rex::kernel::crt {

// ---------------------------------------------------------------------------
// C string operations
// ---------------------------------------------------------------------------

static int native_strncmp(const char* s1, const char* s2, size_t n) {
  return std::strncmp(s1, s2, n);
}

static char* native_strncpy(char* dst, const char* src, size_t n) {
  return std::strncpy(dst, src, n);
}

static char* native_strchr(const char* s, int c) {
  return const_cast<char*>(std::strchr(s, c));
}

static char* native_strstr(const char* haystack, const char* needle) {
  return const_cast<char*>(std::strstr(haystack, needle));
}

static char* native_strrchr(const char* s, int c) {
  return const_cast<char*>(std::strrchr(s, c));
}

static char* native_strtok(char* s, const char* delim) {
  return std::strtok(s, delim);
}

static int native_stricmp(const char* s1, const char* s2) {
#if REX_PLATFORM_WIN32
  return _stricmp(s1, s2);
#else
  return strcasecmp(s1, s2);
#endif
}

static int native_strcpy_s(char* dst, size_t dstsz, const char* src) {
  if (!dst || !src || dstsz == 0)
    return 22;  // EINVAL
#if REX_PLATFORM_WIN32
  return strcpy_s(dst, dstsz, src);
#else
  const size_t src_len = std::strlen(src);
  if (src_len + 1 > dstsz) {
    dst[0] = '\0';
    return 34;  // ERANGE
  }
  std::memcpy(dst, src, src_len + 1);
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// Win32 string functions (lstr*)
// ---------------------------------------------------------------------------

static int native_lstrlenA(const char* s) {
  return s ? static_cast<int>(std::strlen(s)) : 0;
}

static char* native_lstrcpyA(char* dst, const char* src) {
  return std::strcpy(dst, src);
}

static char* native_lstrcpynA(char* dst, const char* src, int maxlen) {
  if (maxlen <= 0)
    return dst;
  std::strncpy(dst, src, maxlen - 1);
  dst[maxlen - 1] = '\0';
  return dst;
}

static char* native_lstrcatA(char* dst, const char* src) {
  return std::strcat(dst, src);
}

static int native_lstrcmpiA(const char* s1, const char* s2) {
#if REX_PLATFORM_WIN32
  return _stricmp(s1, s2);
#else
  return strcasecmp(s1, s2);
#endif
}

}  // namespace rex::kernel::crt

REXCRT_EXPORT(rexcrt_strncmp, rex::kernel::crt::native_strncmp)
REXCRT_EXPORT(rexcrt_strncpy, rex::kernel::crt::native_strncpy)
REXCRT_EXPORT(rexcrt_strchr, rex::kernel::crt::native_strchr)
REXCRT_EXPORT(rexcrt_strstr, rex::kernel::crt::native_strstr)
REXCRT_EXPORT(rexcrt_strrchr, rex::kernel::crt::native_strrchr)
REXCRT_EXPORT(rexcrt_strtok, rex::kernel::crt::native_strtok)
REXCRT_EXPORT(rexcrt__stricmp, rex::kernel::crt::native_stricmp)
REXCRT_EXPORT(rexcrt_strcpy_s, rex::kernel::crt::native_strcpy_s)
REXCRT_EXPORT(rexcrt_lstrlenA, rex::kernel::crt::native_lstrlenA)
REXCRT_EXPORT(rexcrt_lstrcpyA, rex::kernel::crt::native_lstrcpyA)
REXCRT_EXPORT(rexcrt_lstrcpynA, rex::kernel::crt::native_lstrcpynA)
REXCRT_EXPORT(rexcrt_lstrcatA, rex::kernel::crt::native_lstrcatA)
REXCRT_EXPORT(rexcrt_lstrcmpiA, rex::kernel::crt::native_lstrcmpiA)
