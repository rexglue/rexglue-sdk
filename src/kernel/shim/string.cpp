#include <cstring>

#include <rex/platform.h>

#include <rex/hook.h>
#include <rex/kernel/shim/function.h>
#include <rex/string.h>

namespace rex::kernel::shim {

REX_SHIM_FN(strncmp, int, const char* s1, const char* s2, u32 n) {
  return std::strncmp(s1, s2, n);
}

REX_SHIM_FN(strncpy, char*, char* dst, const char* src, u32 n) {
  const size_t length = strnlen(src, n);
  std::memcpy(dst, src, length);
  std::memset(dst + length, 0, n - length);
  return dst;
}

REX_SHIM_FN(strchr, char*, const char* s, int c) {
  return const_cast<char*>(std::strchr(s, c));
}

REX_SHIM_FN(strstr, char*, const char* haystack, const char* needle) {
  return const_cast<char*>(std::strstr(haystack, needle));
}

REX_SHIM_FN(strrchr, char*, const char* s, int c) {
  return const_cast<char*>(std::strrchr(s, c));
}

REX_SHIM_FN(strtok, char*, char* s, const char* delim) {
  // Non reentrant by libc contract, concurrent threads share the context.
  static char* context = nullptr;
#if REX_PLATFORM_WIN32
  return strtok_s(s, delim, &context);
#else
  return strtok_r(s, delim, &context);
#endif
}

REX_SHIM_FN(_stricmp, int, const char* s1, const char* s2) {
  return rex::CompareIgnoreCase(s1, s2);
}

REX_SHIM_FN(strcpy_s, int, char* dst, u32 dstsz, const char* src) {
#if REX_PLATFORM_WIN32
  return strcpy_s(dst, dstsz, src);
#else
  if (!dst || !src || dstsz == 0)
    return 22;
  const size_t src_len = std::strlen(src);
  if (src_len + 1 > dstsz) {
    dst[0] = '\0';
    return 34;
  }
  std::memcpy(dst, src, src_len + 1);
  return 0;
#endif
}

REX_SHIM_FN(lstrcmpiA, int, const char* s1, const char* s2) {
  return rex::CompareIgnoreCase(s1, s2);
}

REX_SHIM_FN(wcslen, u32, const i16* str) {
  const i16* last = str;
  while (*last++)
    ;
  return last - str - 1;
}

REX_SHIM_FN(wcscmp, u32, const i16* lhs, const i16* rhs) {
  while (*lhs && (*lhs == *rhs)) {
    lhs++;
    rhs++;
  }
  return static_cast<int>(*lhs) - static_cast<int>(*rhs);
}

REX_SHIM_FN(wcsncmp, u32, const i16* lhs, const i16* rhs, int count) {
  for (; count > 0; count--, lhs++, rhs++) {
    if (*lhs != *rhs)
      return static_cast<int>(*lhs) - static_cast<int>(*rhs);

    if (*lhs == L'\0')
      return 0;
  }

  return 0;
}

REX_SHIM_FN(wcscoll, u32, const i16* lhs, const i16* rhs) {
  if (lhs && rhs)
    return wcscmp_shim(lhs, rhs);
  return 22;
}

REX_SHIM_FN(wcschr, i16*, const i16* str, i16 ch) {
  while (*str && *str != ch)
    str++;

  return (*str == ch) ? const_cast<i16*>(str) : 0;
}

REX_SHIM_FN(wcsrchr, i16*, const i16* str, i16 ch) {
  const i16* res = 0;

  for (; *str; str++)
    if (*str == ch)
      res = str;

  return const_cast<i16*>(ch ? res : str);
}

REX_SHIM_FN(wcscpy, i16*, const i16* src, i16* dst) {
  i16* d = dst;

  while ((*d++ = *src++))
    ;

  return dst;
}

REX_SHIM_FN(wcsncpy, i16*, i16* dest, const i16* src, int count) {
  i16* d = dest;
  while (count-- && (*d++ = *src++))
    ;
  while (count--)
    *d++ = 0;
  return dest;
}

REX_SHIM_FN(wcsncpy_s, u32, i16* dst, u32 dstsz, const i16* src, u32 count) {
  if (!dst || !src || dstsz == 0)
    return 22;

  u32 i = 0;

  for (; i < count && i + 1 < dstsz && src[i]; i++)
    dst[i] = src[i];

  if (i < dstsz)
    dst[i] = 0;
  else
    dst[dstsz - 1] = 0;

  if (i < count && src[i] != 0)
    return 34;

  return 0;
}

REX_SHIM_FN(wcsstr, i16*, const i16* dest, const i16* src) {
  if (!*src)
    return const_cast<i16*>(dest);

  for (; *dest; dest++) {
    const i16* d = dest;
    const i16* s = src;

    while (*d && *s && *d == *s) {
      d++;
      s++;
    }

    if (!*s)
      return const_cast<i16*>(dest);
  }

  return 0;
}

}  // namespace rex::kernel::shim
