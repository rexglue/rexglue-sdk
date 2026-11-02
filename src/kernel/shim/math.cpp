/**
 * @file        kernel/shim/math.cpp
 * @brief       Native math and number parsing hooks that replace recompiled PPC
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <cstdlib>

#include <rex/platform.h>

#include <locale.h>
#if REX_PLATFORM_MAC
#include <xlocale.h>
#endif

#include <rex/ppc/hook.h>
#include "kernel/shim/function.h"

namespace rex::kernel::shim {

namespace {

f64 ParseDouble(const char* text) {
  char* end = nullptr;
#if REX_PLATFORM_WIN32
  static const _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
  return _strtod_l(text, &end, c_locale);
#else
  static const locale_t c_locale = newlocale(LC_ALL_MASK, "C", nullptr);
  return strtod_l(text, &end, c_locale);
#endif
}

}  // namespace

REX_SHIM_FN(atan2, f64, f64 y, f64 x) {
  return std::atan2(y, x);
}

REX_SHIM_FN(atof, f64, const char* text) {
  return text ? ParseDouble(text) : 0.0;
}

}  // namespace rex::kernel::shim
