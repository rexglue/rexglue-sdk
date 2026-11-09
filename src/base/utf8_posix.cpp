#include <rex/platform.h>

static_assert(!REX_PLATFORM_WIN32, "This file is POSIX-only");

#include <cerrno>
#include <cstdint>
#include <string>

#include <iconv.h>

#include <rex/string/text.h>

namespace rex::string {

namespace {

bool IsInvalidConverter(iconv_t converter) {
  return reinterpret_cast<std::uintptr_t>(converter) == ~std::uintptr_t{0};
}

size_t UTF8SequenceLength(u8 lead, size_t available) {
  size_t length = 1;
  if (lead >= 0xF0 && lead <= 0xF7) {
    length = 4;
  } else if (lead >= 0xE0) {
    length = lead <= 0xEF ? 3 : 1;
  } else if (lead >= 0xC0) {
    length = 2;
  }
  return length < available ? length : available;
}

std::string Convert(const char* to, const char* from, const std::string_view source,
                    bool utf8_source) {
  if (source.empty()) {
    return {};
  }
  const iconv_t converter = iconv_open(to, from);
  if (IsInvalidConverter(converter)) {
    return std::string(source);
  }
  std::string result;
  char* in = const_cast<char*>(source.data());
  size_t in_left = source.size();
  char buffer[1024];
  while (in_left > 0) {
    char* out = buffer;
    size_t out_left = sizeof(buffer);
    const size_t status = iconv(converter, &in, &in_left, &out, &out_left);
    const int error = errno;
    result.append(buffer, static_cast<size_t>(out - buffer));
    if (status != static_cast<size_t>(-1)) {
      break;
    }
    if (error == E2BIG) {
      continue;
    }
    if (error != EILSEQ && error != EINVAL) {
      iconv_close(converter);
      return std::string(source);
    }
    size_t skipped = 1;
    if (utf8_source) {
      skipped = error == EINVAL ? in_left : UTF8SequenceLength(static_cast<u8>(*in), in_left);
      result.push_back('?');
    }
    in += skipped;
    in_left -= skipped;
  }
  iconv_close(converter);
  return result;
}

}  // namespace

std::string ShiftJISToUTF8(const std::string_view source) {
  return Convert("UTF-8", "CP932", source, false);
}

std::string UTF8ToShiftJIS(const std::string_view source) {
  return Convert("CP932", "UTF-8", source, true);
}

}  // namespace rex::string
