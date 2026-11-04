/**
 * @file        tests/unit/kernel/setting_keys_test.cpp
 * @brief       Setting keys and defaults
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>

#include <rex/settings_registry.h>

#include "kernel/title_fixture.h"
#include "temp_folder.h"

namespace {

struct Expected {
  const char* key;
  const char* default_text;
};

constexpr Expected kSettings[] = {
    {"app.name", ""},
    {"app.root", ""},
    {"app.config_file", "{app.root}/{app.name}.toml"},
    {"app.log_root", "{app.root}/logs"},
    {"app.user_root", "{app.root}/user"},
    {"app.cache_root", ""},
    {"app.single_instance", ""},
    {"apu.driver", "sdl3"},
    {"apu.mute", "false"},
    {"apu.mute_unfocused", "false"},
    {"apu.gain", "1"},
    {"apu.center", "1"},
    {"apu.surround", "1"},
    {"apu.lfe", "1"},
    {"apu.peak_limit", "0"},
    {"apu.max_queued_frames", "8"},
    {"gpu.driver", "any"},
    {"gpu.vsync", "true"},
    {"gpu.frame_limit", "0"},
    {"gpu.resolution_scale_x", "1"},
    {"gpu.resolution_scale_y", "1"},
    {"gpu.anisotropic_override", "3"},
    {"hid.drivers", "sdl3"},
    {"hid.shared_pads", "false"},
    {"hid.vibration", "true"},
    {"hid.guide_button", "false"},
    {"hid.left_stick_deadzone", "0"},
    {"hid.right_stick_deadzone", "0"},
    {"hid.mnk_mode", "false"},
    {"hid.mnk_mouse", "false"},
    {"hid.mnk_sensitivity", "1"},
    {"hid.mnk_passthrough", "false"},
    {"title.game_root", ""},
    {"title.game_writable", "false"},
    {"title.update_root", ""},
    {"title.metadata_root", "{app.root}/metadata"},
    {"title.entry", "default.xex"},
    {"title.imports", ""},
    {"runtime.heap_size_mb", "256"},
    {"runtime.title_memory_mb", "480"},
    {"runtime.writable_code_segments", "false"},
    {"runtime.protect_zero", "true"},
    {"runtime.protect_on_release", "false"},
    {"runtime.scribble_heap", "false"},
    {"runtime.ignore_thread_priorities", "true"},
    {"runtime.ignore_thread_affinities", "true"},
    {"runtime.ffmpeg_verbose", "false"},
    {"runtime.terminate_timeout_ms", "2000"},
    {"kernel.headless", "false"},
    {"kernel.user_language", "1"},
    {"kernel.user_country", "103"},
    {"kernel.license_mask", "0"},
    {"kernel.command_line", ""},
    {"log.level", "info"},
    {"log.flush_level", "warn"},
    {"log.flush_interval", "1"},
    {"log.console", "true"},
    {"log.debugger", "true"},
    {"log.file", "true"},
    {"log.file_name", ""},
    {"log.folder_budget", "0"},
    {"log.pattern", "%^%Y-%m-%d %H:%M:%S.%e %Q %-10n %j %v%$"},
    {"log.color", "false"},
    {"ui.driver", "sdl3"},
    {"ui.title", ""},
    {"ui.window_width", "0"},
    {"ui.window_height", "0"},
    {"ui.fullscreen", "true"},
    {"ui.fullscreen_exclusive", "false"},
    {"ui.monitor", "0"},
    {"ui.min_width", "0"},
    {"ui.min_height", "0"},
    {"ui.aspect_ratio", "stretch"},
    {"ui.resolution", ""},
    {"ui.video_mode_width", "1280"},
    {"ui.video_mode_height", "720"},
    {"ui.video_mode_refresh_rate", "60"},
    {"ui.cursor_timeout", "0"},
    {"clock.no_scaling", "false"},
    {"perf.profiler", "false"},
    {"perf.alloc_stack_depth", "0"},
};

}  // namespace

REX_TEST_CASE("every setting keeps its key after the owners moved", "[kernel][settings]") {
  rex::test::TempFolder folder("rex_setting_keys_");
  rex::test::KernelHost host(folder.path);
  for (const Expected& expected : kSettings) {
    INFO(expected.key);
    auto found = REX_SETTINGS().Find(expected.key);
    REQUIRE(found.has_value());
    CHECK((*found)->GetDefaultText() == expected.default_text);
  }
}
