/**
 * @file        base/core_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/path_settings.h>
#include <rex/setting.h>

namespace rex {

class ClockSettings final : public SettingOwner {
 public:
  ClockSettings() : SettingOwner("clock") {}

  REX_SETTING(bool, no_scaling, false,
              "Follows host time and ignores the guest time scalar when true");
};

class PerfSettings final : public SettingOwner {
 public:
  PerfSettings() : SettingOwner("perf") {}

  REX_SETTING(bool, profiler, false, "Start the Tracy profiler at startup when the build has it",
              kStartupOnly);
  REX_SETTING(i32, alloc_stack_depth, 0,
              "Call stack frames Tracy records for each memory allocation, 0 for none",
              Range{0, 32});
};

REX_API ClockSettings& GetClockSettings();
REX_API PerfSettings& GetPerfSettings();

namespace detail {

inline void RegisterCoreSettings() {
  (void)GetClockSettings();
  (void)GetPerfSettings();
  (void)REX_PATHS();
}

}  // namespace detail

}  // namespace rex
