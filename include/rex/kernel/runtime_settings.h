/**
 * @file        kernel/runtime_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::kernel {

class REX_KERNEL_API RuntimeSettings final : public SettingOwner {
 public:
  REX_SETTING(u32, heap_size_mb, 256, "Shim heap size in MiB", Range{1, 2048}, kUntilLaunch);
  REX_SETTING(u32, title_memory_mb, 480, "Title memory in MiB, 0 removes the cap", Range{0, 512},
              kUntilLaunch);
  REX_SETTING(bool, writable_code_segments, false, "Load code and read only data writable",
              kUntilLaunch);
  REX_SETTING(bool, protect_zero, true, "The first 64 KiB fault on any access", kUntilLaunch);
  REX_SETTING(bool, protect_on_release, false, "Released memory faults on access", kUntilLaunch);
  REX_SETTING(bool, scribble_heap, false, "Fill new heap memory with a pattern", kUntilLaunch);
  REX_SETTING(bool, ignore_thread_priorities, true, "Ignore guest thread priorities");
  REX_SETTING(bool, ignore_thread_affinities, true, "Ignore guest thread affinities");
  REX_SETTING(bool, ffmpeg_verbose, false, "Log FFmpeg messages for XMA decoding");
  REX_SETTING(u32, terminate_timeout_ms, 2000, "Time Unload waits for guest threads to exit",
              Range{0, 60000});

  RuntimeSettings();
};

}  // namespace rex::kernel
