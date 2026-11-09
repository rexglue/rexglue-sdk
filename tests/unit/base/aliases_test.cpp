/**
 * @file        tests/unit/base/aliases_test.cpp
 * @brief       Every old cvar name and config field name still reaches a setting or is ignored
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "base/app_harness.h"
#include "app/command_line.h"
#include "gpu/xenos/host/d3d12/d3d12_settings.h"
#include "gpu/xenos/host/vulkan/vulkan_settings.h"
#include "gpu/xenos/xenos_settings.h"

namespace {

using rex::SettingLayer;
using rex::test::HarnessApp;

struct OldName {
  const char* name;
  const char* target; /**< Same as name for a setting, null for a cvar, empty when removed. */
};

constexpr OldName kOldCvars[] = {
    {"apu.max_queued_frames", "apu.max_queued_frames"},
    {"apu.mute", "apu.mute"},
    {"clock.no_scaling", "clock.no_scaling"},
    {"d3d12.allow_tearing", "gpu.d3d12.allow_tearing"},
    {"d3d12.break_on_error", "gpu.d3d12.break_on_error"},
    {"d3d12.break_on_warning", "gpu.d3d12.break_on_warning"},
    {"d3d12.debug", "gpu.d3d12.debug"},
    {"d3d12.queue_priority", "gpu.d3d12.queue_priority"},
    {"gpu.anisotropic_override", "gpu.anisotropic_override"},
    {"gpu.debug_markers", "gpu.d3d12.debug_markers"},
    {"gpu.d3d12_adapter", "gpu.d3d12.adapter"},
    {"bind.debug", "ui.bind.debug"},
    {"bind.console", "ui.bind.console"},
    {"bind.settings", "ui.bind.settings"},
    {"bind.achievements", "ui.bind.achievements"},
    {"gpu.vulkan_device", "gpu.vulkan.device"},
    {"gpu.present_allow_overscan_cutoff", "gpu.xenos.present_allow_overscan_cutoff"},
    {"gpu.present_dither", "gpu.xenos.present_dither"},
    {"gpu.present_letterbox", "gpu.xenos.present_letterbox"},
    {"gpu.present_safe_area_x", "gpu.xenos.present_safe_area_x"},
    {"gpu.present_safe_area_y", "gpu.xenos.present_safe_area_y"},
    {"gpu.resolution_scale_x", "gpu.resolution_scale_x"},
    {"gpu.resolution_scale_y", "gpu.resolution_scale_y"},
    {"gpu.vsync", "gpu.vsync"},
    {"hid.guide_button", "hid.guide_button"},
    {"hid.keybind_a", "hid.keybind_a"},
    {"hid.keybind_b", "hid.keybind_b"},
    {"hid.keybind_back", "hid.keybind_back"},
    {"hid.keybind_dpad_down", "hid.keybind_dpad_down"},
    {"hid.keybind_dpad_left", "hid.keybind_dpad_left"},
    {"hid.keybind_dpad_right", "hid.keybind_dpad_right"},
    {"hid.keybind_dpad_up", "hid.keybind_dpad_up"},
    {"hid.keybind_guide", "hid.keybind_guide"},
    {"hid.keybind_left_shoulder", "hid.keybind_left_shoulder"},
    {"hid.keybind_left_trigger", "hid.keybind_left_trigger"},
    {"hid.keybind_lstick_down", "hid.keybind_lstick_down"},
    {"hid.keybind_lstick_left", "hid.keybind_lstick_left"},
    {"hid.keybind_lstick_press", "hid.keybind_lstick_press"},
    {"hid.keybind_lstick_right", "hid.keybind_lstick_right"},
    {"hid.keybind_lstick_up", "hid.keybind_lstick_up"},
    {"hid.keybind_right_shoulder", "hid.keybind_right_shoulder"},
    {"hid.keybind_right_trigger", "hid.keybind_right_trigger"},
    {"hid.keybind_rstick_down", "hid.keybind_rstick_down"},
    {"hid.keybind_rstick_left", "hid.keybind_rstick_left"},
    {"hid.keybind_rstick_press", "hid.keybind_rstick_press"},
    {"hid.keybind_rstick_right", "hid.keybind_rstick_right"},
    {"hid.keybind_rstick_up", "hid.keybind_rstick_up"},
    {"hid.keybind_start", "hid.keybind_start"},
    {"hid.keybind_x", "hid.keybind_x"},
    {"hid.keybind_y", "hid.keybind_y"},
    {"hid.left_stick_deadzone", "hid.left_stick_deadzone"},
    {"hid.mnk_mode", "hid.mnk_mode"},
    {"hid.mnk_mouse", "hid.mnk_mouse"},
    {"hid.mnk_passthrough", "hid.mnk_passthrough"},
    {"hid.mnk_sensitivity", "hid.mnk_sensitivity"},
    {"hid.right_stick_deadzone", "hid.right_stick_deadzone"},
    {"hid.vibration", "hid.vibration"},
    {"log.file", "log.file"},
    {"log.flush_interval", "log.flush_interval"},
    {"log.level", "log.level"},
    {"perf.alloc_stack_depth", "perf.alloc_stack_depth"},
    {"perf.profiler", "perf.profiler"},
    {"runtime.ffmpeg_verbose", "runtime.ffmpeg_verbose"},
    {"runtime.ignore_thread_affinities", "runtime.ignore_thread_affinities"},
    {"runtime.ignore_thread_priorities", "runtime.ignore_thread_priorities"},
    {"runtime.protect_on_release", "runtime.protect_on_release"},
    {"runtime.protect_zero", "runtime.protect_zero"},
    {"runtime.scribble_heap", "runtime.scribble_heap"},
    {"runtime.writable_code_segments", "runtime.writable_code_segments"},
    {"ui.cursor_timeout", "ui.cursor_timeout"},
    {"ui.fullscreen", "ui.fullscreen"},
    {"ui.fullscreen_exclusive", "ui.fullscreen_exclusive"},
    {"ui.monitor", "ui.monitor"},
    {"ui.resolution", "ui.resolution"},
    {"ui.video_mode_height", "ui.video_mode_height"},
    {"ui.video_mode_refresh_rate", "ui.video_mode_refresh_rate"},
    {"ui.video_mode_width", "ui.video_mode_width"},
    {"ui.window_height", "ui.window_height"},
    {"ui.window_width", "ui.window_width"},
    {"vulkan.allow_present_mode_fifo_relaxed", "gpu.vulkan.allow_present_mode_fifo_relaxed"},
    {"vulkan.allow_present_mode_immediate", "gpu.vulkan.allow_present_mode_immediate"},
    {"vulkan.allow_present_mode_mailbox", "gpu.vulkan.allow_present_mode_mailbox"},
    {"vulkan.log_debug_messages", "gpu.vulkan.log_debug_messages"},
    {"vulkan.prefer_fill_mode_non_solid", "gpu.vulkan.prefer_fill_mode_non_solid"},
    {"vulkan.prefer_fragment_stores_and_atomics", "gpu.vulkan.prefer_fragment_stores_and_atomics"},
    {"vulkan.prefer_geometry_shader", "gpu.vulkan.prefer_geometry_shader"},
    {"vulkan.prefer_vertex_pipeline_stores_and_atomics",
     "gpu.vulkan.prefer_vertex_pipeline_stores_and_atomics"},
    {"vulkan.render_pass_clear", "gpu.vulkan.render_pass_clear"},
    {"vulkan.require_fill_mode_non_solid", "gpu.vulkan.require_fill_mode_non_solid"},
    {"vulkan.require_geometry_shader", "gpu.vulkan.require_geometry_shader"},
    {"vulkan.validation", "gpu.vulkan.validation"},
    {"xenos.allow_invalid_fetch_constants", "gpu.xenos.allow_invalid_fetch_constants"},
    {"xenos.clear_memory_page_state", "gpu.xenos.clear_memory_page_state"},
    {"xenos.d3d12_bindless", "gpu.d3d12.bindless"},
    {"xenos.d3d12_dxbc_disasm", "gpu.d3d12.dxbc_disasm"},
    {"xenos.d3d12_dxbc_disasm_dxilconv", "gpu.d3d12.dxbc_disasm_dxilconv"},
    {"xenos.d3d12_native_stencil_value_output_intel",
     "gpu.d3d12.native_stencil_value_output_intel"},
    {"xenos.d3d12_pipeline_creation_threads", "gpu.d3d12.pipeline_creation_threads"},
    {"xenos.d3d12_readback_memexport", "gpu.d3d12.readback_memexport"},
    {"xenos.d3d12_readback_resolve", "gpu.d3d12.readback_resolve"},
    {"xenos.d3d12_render_target_path", "gpu.d3d12.render_target_path"},
    {"xenos.d3d12_submit_on_primary_buffer_end", "gpu.d3d12.submit_on_primary_buffer_end"},
    {"xenos.d3d12_tessellation_wireframe", "gpu.d3d12.tessellation_wireframe"},
    {"xenos.d3d12_tiled_shared_memory", "gpu.d3d12.tiled_shared_memory"},
    {"xenos.depth_float24_convert_in_pixel_shader",
     "gpu.xenos.depth_float24_convert_in_pixel_shader"},
    {"xenos.depth_float24_round", "gpu.xenos.depth_float24_round"},
    {"xenos.depth_transfer_not_equal_test", "gpu.xenos.depth_transfer_not_equal_test"},
    {"xenos.direct_host_resolve", "gpu.xenos.direct_host_resolve"},
    {"xenos.draw_resolution_scaled_texture_offsets",
     "gpu.d3d12.draw_resolution_scaled_texture_offsets"},
    {"xenos.dump_shaders", "gpu.xenos.dump_shaders"},
    {"xenos.dxbc_source_map", "gpu.d3d12.dxbc_source_map"},
    {"xenos.dxbc_switch", "gpu.d3d12.dxbc_switch"},
    {"xenos.execute_unclipped_draw_vs_on_cpu", "gpu.xenos.execute_unclipped_draw_vs_on_cpu"},
    {"xenos.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
     "gpu.xenos.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend"},
    {"xenos.execute_unclipped_draw_vs_on_cpu_with_scissor",
     "gpu.xenos.execute_unclipped_draw_vs_on_cpu_with_scissor"},
    {"xenos.force_convert_line_loops_to_strips", "gpu.xenos.force_convert_line_loops_to_strips"},
    {"xenos.force_convert_quad_lists_to_triangle_lists",
     "gpu.xenos.force_convert_quad_lists_to_triangle_lists"},
    {"xenos.force_convert_triangle_fans_to_lists",
     "gpu.xenos.force_convert_triangle_fans_to_lists"},
    {"xenos.gamma_render_target_as_unorm16", "gpu.xenos.gamma_render_target_as_unorm16"},
    {"xenos.half_pixel_offset", "gpu.xenos.half_pixel_offset"},
    {"xenos.mrt_edram_used_range_clamp_to_min", "gpu.xenos.mrt_edram_used_range_clamp_to_min"},
    {"xenos.native_2x_msaa", "gpu.xenos.native_2x_msaa"},
    {"xenos.native_stencil_value_output", "gpu.d3d12.native_stencil_value_output"},
    {"xenos.occlusion_query_enable", "gpu.xenos.occlusion_query_enable"},
    {"xenos.pre_mask_resolve_l2_block", "gpu.xenos.pre_mask_resolve_l2_block"},
    {"xenos.present_from_non_ui_thread", ""},
    {"xenos.primitive_processor_cache_min_indices",
     "gpu.xenos.primitive_processor_cache_min_indices"},
    {"xenos.query_occlusion_fake_sample_count", "gpu.xenos.query_occlusion_fake_sample_count"},
    {"xenos.readback_memexport", "gpu.xenos.readback_memexport"},
    {"xenos.readback_memexport_fast", "gpu.xenos.readback_memexport_fast"},
    {"xenos.readback_resolve_half_pixel_offset", "gpu.xenos.readback_resolve_half_pixel_offset"},
    {"xenos.resolve_resolution_scale_fill_half_pixel_offset",
     "gpu.xenos.resolve_resolution_scale_fill_half_pixel_offset"},
    {"xenos.snorm16_render_target_full_range", "gpu.xenos.snorm16_render_target_full_range"},
    {"xenos.store_shaders", "gpu.xenos.store_shaders"},
    {"xenos.swap_post_effect", "gpu.xenos.swap_post_effect"},
    {"xenos.texture_3d_to_2d", "gpu.xenos.texture_3d_to_2d"},
    {"xenos.texture_cache_memory_limit_hard", "gpu.xenos.texture_cache_memory_limit_hard"},
    {"xenos.texture_cache_memory_limit_render_to_texture",
     "gpu.xenos.texture_cache_memory_limit_render_to_texture"},
    {"xenos.texture_cache_memory_limit_soft", "gpu.xenos.texture_cache_memory_limit_soft"},
    {"xenos.texture_cache_memory_limit_soft_lifetime",
     "gpu.xenos.texture_cache_memory_limit_soft_lifetime"},
    {"xenos.vulkan_async_skip_incomplete_frames", "gpu.vulkan.async_skip_incomplete_frames"},
    {"xenos.vulkan_cube_map_edge_filtering_off", "gpu.vulkan.cube_map_edge_filtering_off"},
    {"xenos.vulkan_dynamic_rendering", "gpu.vulkan.dynamic_rendering"},
    {"xenos.vulkan_force_convert_quad_lists_to_triangle_lists",
     "gpu.vulkan.force_convert_quad_lists_to_triangle_lists"},
    {"xenos.vulkan_force_expand_point_sprites_in_vs",
     "gpu.vulkan.force_expand_point_sprites_in_vs"},
    {"xenos.vulkan_force_expand_rectangle_lists_in_vs",
     "gpu.vulkan.force_expand_rectangle_lists_in_vs"},
    {"xenos.vulkan_readback_memexport", "gpu.vulkan.readback_memexport"},
    {"xenos.vulkan_readback_resolve", "gpu.vulkan.readback_resolve"},
    {"xenos.vulkan_render_target_path", "gpu.vulkan.render_target_path"},
    {"xenos.vulkan_sparse_shared_memory", "gpu.vulkan.sparse_shared_memory"},
    {"xenos.vulkan_submit_on_primary_buffer_end", "gpu.vulkan.submit_on_primary_buffer_end"},
    {"xenos.vulkan_tessellation_wireframe", "gpu.vulkan.tessellation_wireframe"},
    {"xenos.use_fuzzy_alpha_epsilon", "gpu.d3d12.use_fuzzy_alpha_epsilon"},
    {"xenos.vulkan_pipeline_creation_threads", "gpu.vulkan.pipeline_creation_threads"},
    {"audio_mute", "apu.mute"},
    {"clock_no_scaling", "clock.no_scaling"},
    {"log_level", "log.level"},
    {"log_file", "log.file_name"},
    {"log_flush_interval", "log.flush_interval"},
    {"vibration", "hid.vibration"},
    {"keybind_dpad_up", "hid.keybind_dpad_up"},
    {"protect_zero", "runtime.protect_zero"},
    {"window_width", "ui.window_width"},
};

constexpr OldName kOldFields[] = {
    {"paths.game_data_root", "title.game_root"},
    {"paths.user_data_root", "app.user_root"},
    {"paths.update_data_root", "title.update_root"},
    {"paths.cache_root", "app.cache_root"},
    {"paths.metadata_root", "title.metadata_root"},
    {"paths.config_file", "app.config_file"},
    {"runtime.heap_size_mb", "runtime.heap_size_mb"},
    {"runtime.title_memory_mb", "runtime.title_memory_mb"},
    {"runtime.writable_code_segments", "runtime.writable_code_segments"},
    {"runtime.protect_zero", "runtime.protect_zero"},
    {"runtime.terminate_timeout", "runtime.terminate_timeout_ms"},
    {"runtime.kernel.headless", "kernel.headless"},
    {"runtime.kernel.user_language", "kernel.user_language"},
    {"runtime.kernel.user_country", "kernel.user_country"},
    {"runtime.kernel.license_mask", "kernel.license_mask"},
    {"runtime.kernel.command_line", "kernel.command_line"},
    {"filesystem.game_mount_flags", ""},
    {"gpu.driver", "gpu.driver"},
    {"gpu.resolution_scale_x", "gpu.resolution_scale_x"},
    {"gpu.resolution_scale_y", "gpu.resolution_scale_y"},
    {"profiler", "perf.profiler"},
    {"single_instance", "app.single_instance"},
};

struct XenosOwner : rex::gpu::XenosSettings {};

using VulkanOwner = rex::gpu::vulkan::VulkanSettings;
using D3D12Owner = rex::gpu::d3d12::D3D12Settings;

struct Owners {
  XenosOwner xenos;
  VulkanOwner vulkan;
  D3D12Owner d3d12;
  Owners() {
    REQUIRE(REX_SETTINGS().Register(xenos));
    REQUIRE(REX_SETTINGS().Register(vulkan));
    REQUIRE(REX_SETTINGS().Register(d3d12));
  }
  ~Owners() {
    REX_SETTINGS().Unregister(d3d12);
    REX_SETTINGS().Unregister(vulkan);
    REX_SETTINGS().Unregister(xenos);
  }
};

size_t Count(rex::log::LogCaptureSink& sink, const std::string& text) {
  std::vector<rex::log::CapturedMessage> entries;
  sink.CopySince(0, entries);
  return static_cast<size_t>(std::ranges::count_if(
      entries, [&](const auto& entry) { return entry.text.find(text) != std::string::npos; }));
}

void CheckLanding(const OldName& old) {
  const std::string name = old.name;
  INFO(name);
  if (old.target == nullptr) {
    CHECK_FALSE(REX_SETTINGS().Find(name));
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, name, "1"));
    return;
  }
  if (std::string_view(old.target).empty()) {
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, name, "1"));
    return;
  }
  auto target = REX_SETTINGS().Find(old.target);
  if (!target) {
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, name, "1"));
    return;
  }
  const std::string value = (*target)->GetDefaultText();
  CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, name, value));
  CHECK((*target)->GetLayer() == SettingLayer::kFile);
}

void CheckNotice(rex::log::LogCaptureSink& sink, const OldName& old) {
  const std::string name = old.name;
  INFO(name);
  if (old.target == nullptr || name == old.target) {
    return;
  }
  const std::string_view target = old.target;
  const std::string text = target.empty() ? name + " was removed and is ignored"
                                          : name + " is now " + std::string(target);
  CHECK(Count(sink, text) == 1);
}

}  // namespace

REX_TEST_CASE("aliases cover every old name", "[base][aliases]") {
  HarnessApp app;
  rex::log::LogCaptureSink sink;
  rex::ScopeGuard sink_guard;
  Owners owners;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    sink_guard = REX_LOGGER().AddSink(sink);
    for (const OldName& old : kOldCvars) {
      CheckLanding(old);
    }
    for (const OldName& old : kOldFields) {
      CheckLanding(old);
    }
    return rex::Ok();
  };
  app.init = [&]() -> rex::Result<void> {
    for (const OldName& old : kOldCvars) {
      CheckNotice(sink, old);
    }
    for (const OldName& old : kOldFields) {
      CheckNotice(sink, old);
    }
    sink_guard.Reset();
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("old log.file path converts", "[base][aliases]") {
  HarnessApp app;
  rex::log::LogCaptureSink sink;
  rex::ScopeGuard sink_guard;
  const std::filesystem::path logs = app.folder / "oldlogs";
  const std::filesystem::path file = logs / "old.log";
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    sink_guard = REX_LOGGER().AddSink(sink);
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, "log.file", file.string()));
    return rex::Ok();
  };
  app.init = [&]() -> rex::Result<void> {
    CHECK(REX_LOGGER().file.Get());
    CHECK(REX_LOGGER().file.GetLayer() == SettingLayer::kFile);
    CHECK(REX_SETTINGS().Find("app.log_root").value()->GetText() == logs.generic_string());
    CHECK(REX_LOGGER().file_name.Get() == "old.log");
    CHECK(Count(sink, "is now log.file, app.log_root and log.file_name") == 1);
    sink_guard.Reset();
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("old log.file bool keeps its meaning", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, "log.file", "false"));
    return rex::Ok();
  };
  app.init = [&]() -> rex::Result<void> {
    CHECK_FALSE(REX_LOGGER().file.Get());
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("the structural flags map to their settings", "[base][aliases]") {
  HarnessApp app;
  rex::log::LogCaptureSink sink;
  Owners owners;
  const std::string base = app.folder.generic_string();
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    auto sink_guard = REX_LOGGER().AddSink(sink);
    const std::vector<std::string> args = {"--modules=rexfilesystem",
                                           "--game_data_root=" + base + "/game",
                                           "--user_data_root=" + base + "/user",
                                           "--update_data_root=" + base + "/update",
                                           "--cache_root=" + base + "/cache",
                                           "--metadata_root=" + base + "/meta",
                                           "--config_file=" + base + "/x.toml",
                                           "--gpu.driver=any"};
    CHECK(rex::app::detail::ParseCommandLine(rex::CLIArgList::Parse(args)));
    CHECK(Count(sink, "--modules sets no setting, drivers are chosen by driver settings") == 1);
    const std::pair<const char*, std::string> expected[] = {{"title.game_root", base + "/game"},
                                                            {"app.user_root", base + "/user"},
                                                            {"title.update_root", base + "/update"},
                                                            {"app.cache_root", base + "/cache"},
                                                            {"title.metadata_root", base + "/meta"},
                                                            {"app.config_file", base + "/x.toml"},
                                                            {"gpu.driver", "any"}};
    for (const auto& [name, text] : expected) {
      INFO(name);
      auto found = REX_SETTINGS().Find(name);
      REQUIRE(found);
      CHECK((*found)->GetLayer() == SettingLayer::kCommandLine);
      CHECK((*found)->GetText() == text);
    }
    sink_guard.Reset();
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("an old flat name on the command line sets the new setting", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    const std::vector<std::string> args = {"--log_flush_interval=5"};
    CHECK(rex::app::detail::ParseCommandLine(rex::CLIArgList::Parse(args)));
    auto found = REX_SETTINGS().Find("log.flush_interval");
    REQUIRE(found);
    CHECK((*found)->GetLayer() == SettingLayer::kCommandLine);
    CHECK((*found)->GetText() == "5");
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("an old flat name in a config file sets the new setting", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    CHECK(REX_SETTINGS().SetText(SettingLayer::kFile, "log_flush_interval", "7"));
    auto found = REX_SETTINGS().Find("log.flush_interval");
    REQUIRE(found);
    CHECK((*found)->GetLayer() == SettingLayer::kFile);
    CHECK((*found)->GetText() == "7");
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("an old flat bool name takes the bare form", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    const std::vector<std::string> args = {"--clock_no_scaling"};
    CHECK(rex::app::detail::ParseCommandLine(rex::CLIArgList::Parse(args)));
    auto found = REX_SETTINGS().Find("clock.no_scaling");
    REQUIRE(found);
    CHECK((*found)->GetLayer() == SettingLayer::kCommandLine);
    CHECK((*found)->GetText() == "true");
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("an old flat bool name takes the no form", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    const std::vector<std::string> args = {"--no-clock_no_scaling"};
    CHECK(rex::app::detail::ParseCommandLine(rex::CLIArgList::Parse(args)));
    auto found = REX_SETTINGS().Find("clock.no_scaling");
    REQUIRE(found);
    CHECK((*found)->GetLayer() == SettingLayer::kCommandLine);
    CHECK((*found)->GetText() == "false");
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}

REX_TEST_CASE("a flat name that is no alias stays an unknown option", "[base][aliases]") {
  HarnessApp app;
  app.configure = [&](rex::SettingsRegistry&) -> rex::Result<void> {
    const std::vector<std::string> args = {"--no_such_flat_name=1"};
    CHECK_FALSE(rex::app::detail::ParseCommandLine(rex::CLIArgList::Parse(args)));
    return rex::Ok();
  };
  REQUIRE(app.Start());
  app.Finish();
}
