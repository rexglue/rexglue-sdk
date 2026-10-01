/**
 * @file        runtime/guest_path.cpp
 * @brief       Guest path normalization for Xbox 360 VFS paths
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/runtime/guest_path.h>

#include <algorithm>
#include <cctype>

#include <rex/string/utf8.h>

namespace rex::runtime {

std::string NormalizeGuestPath(std::string_view path) {
  // Manifest / config consumers expect POSIX-style guest paths: forward
  // slashes, no device prefix, lowercase. The runtime VFS tolerates either
  // separator, so converting backslashes here keeps writers (TOML) happy.
  std::string result = rex::utf8_fix_path_separators(path, U'/');

  auto colon = result.find(':');
  if (colon != std::string::npos && colon + 1 < result.size() && result[colon + 1] == '/') {
    result.erase(0, colon + 2);
  }

  result = rex::utf8_canonicalize_path(result, U'/');

  while (!result.empty() && result.front() == '/') {
    result.erase(result.begin());
  }

  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return result;
}

}  // namespace rex::runtime
