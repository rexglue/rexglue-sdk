/**
 * @file        rex/filesystem/guest_name.h
 * @brief       File and folder names from the guest, as parts of host paths.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>

namespace rex::filesystem {

// Whether a name the guest gives a file or folder can be one part of a host
// path: not empty, not "." or "..", no separator ('/' or '\\'), no ':' (a
// drive or a Windows stream) and no control character. Anything else could
// reach outside the folder the guest's device is mounted on.
inline bool IsPlainGuestName(std::string_view name) {
  if (name.empty() || name.size() > 255 || name == "." || name == "..") {
    return false;
  }
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') {
      return false;
    }
  }
  return true;
}

// `name` as one plain host path part: unchanged when IsPlainGuestName allows
// it, else with each character it refuses replaced by '_' ("_" for an empty
// name, "." or "..").
inline std::string PlainGuestName(std::string_view name) {
  if (IsPlainGuestName(name)) {
    return std::string(name);
  }
  std::string out;
  for (unsigned char c : name.substr(0, 255)) {
    out += (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') ? '_' : char(c);
  }
  if (out.empty() || out == "." || out == "..") {
    out = "_";
  }
  return out;
}

}  // namespace rex::filesystem
