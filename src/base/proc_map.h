/**
 * @file        base/proc_map.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <rex/types.h>

namespace rex::memory {

struct ProcMapping {
  u64 start = 0;
  u64 end = 0;
  u64 offset = 0;
  std::array<char, 4> perms{};
  std::string_view path;
};

namespace detail {

inline std::string_view NextMapsField(std::string_view& line) {
  const size_t start = line.find_first_not_of(' ');
  if (start == std::string_view::npos) {
    line = {};
    return {};
  }
  line.remove_prefix(start);
  const size_t end = std::min(line.find(' '), line.size());
  const std::string_view field = line.substr(0, end);
  line.remove_prefix(end);
  return field;
}

inline u64 ParseMapsHex(std::string_view text) {
  u64 value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return value;
}

inline bool ParseMapsLine(std::string_view line, ProcMapping& out) {
  const std::string_view range = NextMapsField(line);
  const std::string_view perms = NextMapsField(line);
  const std::string_view offset = NextMapsField(line);
  NextMapsField(line);
  NextMapsField(line);
  const size_t dash = range.find('-');
  if (dash == std::string_view::npos || perms.size() < out.perms.size()) {
    return false;
  }
  out.start = ParseMapsHex(range.substr(0, dash));
  out.end = ParseMapsHex(range.substr(dash + 1));
  out.offset = ParseMapsHex(offset);
  std::copy_n(perms.begin(), out.perms.size(), out.perms.begin());
  const size_t path_start = line.find_first_not_of(' ');
  out.path = path_start == std::string_view::npos ? std::string_view{} : line.substr(path_start);
  return out.start < out.end;
}

}  // namespace detail

/**
 * Calls fn for each mapping of the process without allocating or locking. Paths longer than the
 * line buffer arrive cut short.
 *
 * @tparam F Callable taking a const ProcMapping& and returning true to continue.
 * @param fn Called per mapping, the path view is valid only during the call.
 * @return True when the map could be read.
 */
template <typename F>
bool ForEachMapping(F&& fn) {
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::array<char, 1024> line{};
  size_t length = 0;
  std::array<char, 4096> chunk{};
  bool more = true;
  const auto deliver = [&]() {
    ProcMapping mapping;
    if (detail::ParseMapsLine({line.data(), length}, mapping)) {
      more = fn(static_cast<const ProcMapping&>(mapping));
    }
    length = 0;
  };
  for (ssize_t count; more && (count = read(fd, chunk.data(), chunk.size())) > 0;) {
    for (ssize_t i = 0; more && i < count; ++i) {
      if (chunk[i] == '\n') {
        deliver();
      } else if (length < line.size()) {
        line[length++] = chunk[i];
      }
    }
  }
  if (more && length) {
    deliver();
  }
  close(fd);
  return true;
}

/**
 * Finds the mapping that holds an address without allocating or locking.
 *
 * @param out The mapping, with an empty path.
 * @return True when a mapping holds the address.
 */
inline bool FindMapping(u64 address, ProcMapping& out) {
  bool found = false;
  ForEachMapping([&](const ProcMapping& mapping) {
    if (address >= mapping.start && address < mapping.end) {
      out = mapping;
      out.path = {};
      found = true;
    }
    return !found;
  });
  return found;
}

}  // namespace rex::memory
