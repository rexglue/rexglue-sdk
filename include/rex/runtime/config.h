/**
 * @file        runtime/config.h
 * @brief       Runtime module slice of rex::Config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rex::runtime {

/** Values the kernel reports to the title. */
struct KernelConfig {
  /** XAM UI exports answer without a dialog. */
  bool headless = false;
  /** XCONFIG_USER_LANGUAGE. */
  uint32_t user_language = 1;
  /** XCONFIG_USER_COUNTRY. */
  uint32_t user_country = 103;
  /** XamContentGetLicenseMask. */
  uint32_t license_mask = 0;
  /** Appended to ExLoadedCommandLine after default.xex. */
  std::string command_line;
};

/** Runtime module slice. */
struct RuntimeConfig {
  /** Guest CRT heap size in MiB, used when the image was recompiled with the rexcrt heap. 1 to
   * 2048.
   */
  uint32_t heap_size_mb = 256;
  /** Values the kernel reports to the title. */
  KernelConfig kernel;
};

}  // namespace rex::runtime
