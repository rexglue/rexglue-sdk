#include <cstring>

#include <rex/platform.h>

#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"
#include <rex/string/text.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_state.h>
#include "kernel/title_state.h"
#include "kernel/kernel.h"

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
  static thread_local char* host_context = nullptr;
  ThreadState* state = ThreadState::Get();
  char*& context = state ? state->GetStrtokContext() : host_context;
#if REX_PLATFORM_WIN32
  return strtok_s(s, delim, &context);
#else
  return strtok_r(s, delim, &context);
#endif
}

REX_SHIM_FN(_stricmp, int, const char* s1, const char* s2) {
  return rex::string::CompareIgnoreCase(s1, s2);
}

REX_SHIM_FN(strcpy_s, int, char* dst, u32 dstsz, const char* src) {
  if (!dst || !src || dstsz == 0)
    return 22;
  const size_t src_len = std::strlen(src);
  if (src_len + 1 > dstsz) {
    dst[0] = '\0';
    return 34;
  }
  std::memcpy(dst, src, src_len + 1);
  return 0;
}

REX_SHIM_FN(lstrcmpiA, int, const char* s1, const char* s2) {
  return rex::string::CompareIgnoreCase(s1, s2);
}

REX_SHIM_FN(wcslen, u32, const be_u16* str) {
  u32 length = 0;
  while (str[length]) {
    ++length;
  }
  return length;
}

REX_SHIM_FN(wcscmp, int, const be_u16* lhs, const be_u16* rhs) {
  while (*lhs && static_cast<u16>(*lhs) == static_cast<u16>(*rhs)) {
    ++lhs;
    ++rhs;
  }
  return static_cast<int>(static_cast<u16>(*lhs)) - static_cast<int>(static_cast<u16>(*rhs));
}

REX_SHIM_FN(wcsncmp, int, const be_u16* lhs, const be_u16* rhs, int count) {
  for (; count > 0; count--, lhs++, rhs++) {
    const u16 left = *lhs;
    const u16 right = *rhs;
    if (left != right) {
      return static_cast<int>(left) - static_cast<int>(right);
    }
    if (left == 0) {
      return 0;
    }
  }
  return 0;
}

REX_SHIM_FN(wcscoll, int, const be_u16* lhs, const be_u16* rhs) {
  if (lhs && rhs) {
    return wcscmp_shim(lhs, rhs);
  }
  return 22;
}

REX_SHIM_FN(wcschr, be_u16*, const be_u16* str, u16 ch) {
  while (*str && static_cast<u16>(*str) != ch) {
    ++str;
  }
  return static_cast<u16>(*str) == ch ? const_cast<be_u16*>(str) : nullptr;
}

REX_SHIM_FN(wcsrchr, be_u16*, const be_u16* str, u16 ch) {
  const be_u16* found = nullptr;
  for (; *str; ++str) {
    if (static_cast<u16>(*str) == ch) {
      found = str;
    }
  }
  return const_cast<be_u16*>(ch ? found : str);
}

REX_SHIM_FN(wcscpy, be_u16*, be_u16* dst, const be_u16* src) {
  be_u16* out = dst;
  while ((*out++ = *src++))
    ;
  return dst;
}

REX_SHIM_FN(wcsncpy, be_u16*, be_u16* dst, const be_u16* src, u32 count) {
  u32 i = 0;
  for (; i < count && src[i]; ++i) {
    dst[i] = src[i];
  }
  for (; i < count; ++i) {
    dst[i] = be_u16(0);
  }
  return dst;
}

REX_SHIM_FN(wcsncpy_s, u32, be_u16* dst, u32 dstsz, const be_u16* src, u32 count) {
  if (!dst || !src || dstsz == 0) {
    return 22;
  }

  u32 i = 0;
  for (; i < count && i + 1 < dstsz && src[i]; i++) {
    dst[i] = src[i];
  }

  if (i < dstsz) {
    dst[i] = be_u16(0);
  } else {
    dst[dstsz - 1] = be_u16(0);
  }

  if (i < count && src[i] != 0) {
    return 34;
  }
  return 0;
}

REX_SHIM_FN(wcsstr, be_u16*, const be_u16* dest, const be_u16* src) {
  if (!*src) {
    return const_cast<be_u16*>(dest);
  }

  for (; *dest; dest++) {
    const be_u16* d = dest;
    const be_u16* s = src;
    while (*d && *s && static_cast<u16>(*d) == static_cast<u16>(*s)) {
      d++;
      s++;
    }
    if (!*s) {
      return const_cast<be_u16*>(dest);
    }
  }
  return nullptr;
}

}  // namespace rex::kernel::shim
