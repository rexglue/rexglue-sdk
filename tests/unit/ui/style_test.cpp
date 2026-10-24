/**
 * @file        tests/unit/ui/style_test.cpp
 * @brief       The default section colors reference the palette roles
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ui/style.h>

using rex::ui::Style;

REX_TEST_CASE("Default section colors reference the palette roles they equal", "[ui][style]") {
  const Style style;
  CHECK(style.settings.warning == style.palette.warning);
  CHECK(style.settings.lifecycle_hot_reload == style.palette.success);
  CHECK(style.settings.lifecycle_init_only == style.palette.error);
  CHECK(style.achievements.header_text == style.palette.info);
  CHECK(style.debug.muted_text == style.palette.text_muted);
  CHECK(style.console.level[5] == style.palette.error);
}
