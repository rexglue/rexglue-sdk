/**
 * @file        tests/unit/base/build_stamp_test.cpp
 * @brief       The build stamp names the version, platform, configuration and configure time
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>

#include <rex/version.h>

REX_TEST_CASE("The build stamp carries the version, platform and configuration", "[base]") {
  const std::string stamp = rex::GetBuildStamp();
  CHECK(
      stamp.starts_with("build: rexglue-v" REXGLUE_VERSION_STRING "-" REXGLUE_BUILD_PLATFORM "-"));
  CHECK(stamp.find('@') != std::string::npos);
  CHECK(stamp.size() - stamp.find('@') == 14);
}