/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstddef>
#include <ostream>
#include <string>

#include <fmt/format.h>

#include <rex/vec128.h>

namespace rex {

std::string to_string(const vec128_t& value) {
  return fmt::format("({}, {}, {}, {})", value.x, value.y, value.z, value.w);
}

std::ostream& operator<<(std::ostream& os, const vec128_t& value) {
  // Inline hex format to avoid string_util dependency
  os << fmt::format("[{:08X} {:08X} {:08X} {:08X}]", value.u32[0], value.u32[1], value.u32[2],
                    value.u32[3]);
  return os;
}

}  // namespace rex
