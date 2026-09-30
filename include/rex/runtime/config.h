/**
 * @file        runtime/config.h
 * @brief       Runtime module config inside rex::Config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <string>

#include <rex/types.h>

namespace rex::runtime {

/** Values the kernel reports to the title. */
struct KernelConfig {
  /** Xam UI exports answer without a dialog. */
  bool headless = false;
  /** XCONFIG_USER_LANGUAGE. */
  u32 user_language = 1;
  /** XCONFIG_USER_COUNTRY. */
  u32 user_country = 103;
  /** XamContentGetLicenseMask. */
  u32 license_mask = 0;
  /** Appended to ExLoadedCommandLine after default.xex. */
  std::string command_line;
};

/** Runtime module config. */
struct RuntimeConfig {
  /** Guest CRT heap size in MiB, used when the image was recompiled with the rexcrt heap. 1 to
   * 2048.
   */
  u32 title_memory_mb = 480;
  /** Code and read only data sections of the title image load writable. */
  bool writable_code_segments = false;
  /** The first and last 64 KiB of the guest address space fault on any access. */
  bool protect_zero = true;
  /** Values the kernel reports to the title. */
  KernelConfig kernel;
  /** Time TerminateTitle waits for guest threads to exit before it terminates the rest. */
  std::chrono::milliseconds terminate_timeout{2000};
};

/** Why the title exited while the app keeps running. */
enum class TitleExitReason : u8 {
  NormalExit,     /**< The title's main thread returned. */
  HardExit,       /**< The title called HalReturnToFirmware. */
  TitleDataError, /**< Xam reported that title data could not be read from the disc. */
};

}  // namespace rex::runtime
