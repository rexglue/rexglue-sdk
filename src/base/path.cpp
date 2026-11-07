#include <rex/string/path.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rex/string/text.h>

#include "base/utf8_codec.h"

namespace rex::string {

std::vector<std::string_view> utf8_split_path(const std::string_view path) {
  return utf8_split(path, "\\/", true);
}

std::string utf8_join_paths(const std::string_view left_path, const std::string_view right_path,
                            char32_t separator) {
  if (left_path.empty()) {
    return std::string(right_path);
  } else if (right_path.empty()) {
    return std::string(left_path);
  }

  CodePointIterator it;
  std::tie(std::ignore, it) = BuildIterators(left_path);
  --it;

  std::string result = std::string(left_path);
  if (*it != static_cast<u32>(separator)) {
    EncodeUTF8(separator, result);
  }
  return result + std::string(right_path);
}

std::string utf8_join_paths(const std::vector<std::string>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_join_paths(const std::vector<std::string_view>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_fix_path_separators(const std::string_view path, char32_t new_separator) {
  if (path.empty()) {
    return std::string();
  }

  const char32_t old_separator = new_separator == U'\\' ? U'/' : U'\\';

  auto [path_begin, path_end] = BuildIterators(path);

  std::string result;
  auto it = path_begin;
  auto last = it;

  auto is_separator = [old_separator, new_separator](char32_t c) {
    return c == static_cast<u32>(old_separator) || c == static_cast<u32>(new_separator);
  };

  if (is_separator(*it)) {
    EncodeUTF8(new_separator, result);
    ++it;
    last = it;
  }

  for (;;) {
    it = std::find_if(it, path_end, is_separator);
    if (it == path_end) {
      break;
    }

    if (it != last) {
      auto offset = ByteLength(path_begin, last);
      auto length = ByteLength(path_begin, it) - offset;
      result += path.substr(offset, length);
      EncodeUTF8(new_separator, result);
    }

    ++it;
    last = it;
  }

  if (last == path_begin) {
    return std::string(path);
  }

  if (last != path_end) {
    auto offset = ByteLength(path_begin, last);
    result += path.substr(offset);
  }

  return result;
}

std::string utf8_find_name_from_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = BuildIterators(path);

  auto it = end;
  --it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  end = std::next(it);

  while (*it != static_cast<u32>(separator)) {
    if (it == begin) {
      break;
    }
    --it;
  }

  if (*it == static_cast<u32>(separator)) {
    ++it;
  }

  auto offset = ByteLength(begin, it);
  auto length = ByteLength(it, end);
  return std::string(path.substr(offset, length));
}

std::string utf8_find_base_name_from_path(const std::string_view path, char32_t separator) {
  auto name = utf8_find_name_from_path(path, separator);
  if (name.empty()) {
    return std::string();
  }

  auto [begin, end] = BuildIterators(name);

  auto it = end;
  --it;

  while (it != begin) {
    if (*it == static_cast<u32>('.')) {
      break;
    }
    --it;
  }

  if (it == begin) {
    return name;
  }

  auto length = ByteLength(it, end);
  auto offset = name.length() - length;
  return std::string(name.substr(0, offset));
}

std::string utf8_find_base_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = BuildIterators(path);

  auto it = end;
  --it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  while (*it != static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  auto sep_pos = it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  // A volume root such as "D:" keeps its separator.
  if (*it == static_cast<u32>(':')) {
    auto length = ByteLength(begin, std::next(sep_pos));
    return std::string(path.substr(0, length));
  }

  auto length = ByteLength(begin, std::next(it));
  return std::string(path.substr(0, length));
}

std::string utf8_canonicalize_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto is_rooted = StartsWithCodePoint(path, separator);

  auto parts = utf8_split_path(path);
  for (auto it = parts.begin(); it != parts.end();) {
    const auto& part = *it;
    if (part == ".") {
      it = parts.erase(it);
    } else if (part == "..") {
      // ".." never removes a device name such as "game:".
      if (it != parts.begin()) {
        auto prev = std::prev(it);
        if (!utf8_ends_with(*prev, ":")) {
          it = parts.erase(prev);
        }
      }
      it = parts.erase(it);
    } else {
      ++it;
    }
  }

  return !is_rooted ? utf8_join_paths(parts, separator)
                    : ToString(separator) + utf8_join_paths(parts, separator);
}

std::string_view utf8_strip_leading_slashes(const std::string_view path) {
  return path.substr(std::min(path.find_first_not_of('/'), path.size()));
}

std::string NormalizeGuestPath(std::string_view path) {
  std::string result = utf8_fix_path_separators(path, U'/');

  auto colon = result.find(':');
  if (colon != std::string::npos && colon + 1 < result.size() && result[colon + 1] == '/') {
    result.erase(0, colon + 2);
  }

  result = utf8_canonicalize_path(result, U'/');

  result.erase(0, result.size() - utf8_strip_leading_slashes(result).size());

  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return result;
}

}  // namespace rex::string
