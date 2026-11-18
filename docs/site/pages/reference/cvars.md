# Settings Reference

This page lists every setting that the SDK registers, and the old setting names that the SDK accepts in their place.

A setting has a full name made of a scope and a name, such as `gpu.vsync`. Set it in the config file, in the environment or on the command line as `--gpu.vsync=false`, or from code. See [Settings](../app/settings.md) for how settings are declared and which source wins. The `title.*` settings use [title](../reference/glossary.md#title), the Xbox 360 term for a game.

## Columns

| Column | Meaning |
|---|---|
| Default | A path default can contain another setting in braces, such as `{app.root}`, which takes the current value of that setting. |
| Restriction | See [Restriction](#restriction). |
| Meaning | Includes the range or the allowed values. Values outside them are rejected. |

### Restriction

A setting with an empty Restriction can change at any time, and the new value applies when the code that uses it next reads it.

| Restriction | Meaning |
|---|---|
| requires restart | Writable at any time. The new value takes effect at the next start. |
| init only | Set in the config file, the environment, the command line or `App::OnConfigure`. Fixed once the systems have started, right before `App::OnInit`. |
| locked in game | Writable while no game runs. Fixed from the launch of the game until it unloads. |

See [When a Setting Can Change](../app/settings.md#when-a-setting-can-change) for the exact points.

### Availability

- A setting exists while its [settings class](../app/settings.md#declaring-settings) exists. Each table gives its settings class.
- A value can be given for a setting before its settings class is registered, for example for a driver setting. The SDK keeps the value and applies it when the class registers.
- A name that no class has declared by the end of startup logs a warning when it came from the config file, and stops startup when it came from the command line.
- The SDK reads an environment variable only for a registered setting.

See [Unknown Names](../app/settings.md#unknown-names).

## Settings

### app

Every program has these settings. `App` declares `app.name`, `app.root`, `app.log_root` and `app.single_instance`, and `PathSettings` declares the other three. See [Folders and Storage](../filesystem/folders-and-storage.md) for how the defaults resolve.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `app.name` | `string` | `""` | init only | Base name of the default config file and the log file. Passed to the `App` constructor and never written to the config file. |
| `app.root` | `path` | `""` | init only | Base folder that the other default paths start from. Empty means the executable folder. |
| `app.config_file` | `path` | `"{app.root}/{app.name}.toml"` | init only | Config file loaded at startup. |
| `app.log_root` | `path` | `"{app.root}/logs"` | init only | Log folder. |
| `app.user_root` | `path` | `"{app.root}/user"` | init only | Saves and per player data. |
| `app.cache_root` | `path` | `""` | init only | Shader and pipeline caches. Empty keeps none. |
| `app.single_instance` | `string` | `""` | init only | A lock name. A second copy with the same name fails to start. Empty turns the check off. |

### clock

Every program has this setting. Declared by `ClockSettings` in `rexbase`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `clock.no_scaling` | `bool` | `false` |  | The guest clock follows host time and ignores the guest time scale. |

### log

Every program has these settings. Declared by [`Logger`](../app/logging.md). Each log category adds one more setting, such as [`log.categories.gpu`](#logcategories).

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `log.level` | `string` | `"info"` |  | Level for categories without their own. `trace`, `dev`, `debug`, `info`, `warn`, `error`, `critical` or `off`. |
| `log.flush_level` | `string` | `"warn"` | init only | Lowest level that flushes every sink immediately. Same values as `log.level`. |
| `log.flush_interval` | `i32` | `1` | init only | Seconds between flushes. 0 to 60. |
| `log.console` | `bool` | `true` | init only | Log to the terminal. |
| `log.debugger` | `bool` | `true` | init only | Log to an attached debugger, on Windows only. |
| `log.file` | `bool` | `true` | init only | Log to a file under `app.log_root`. |
| `log.file_name` | `string` | `""` | init only | Name of the log file. Empty uses the program name. See [Logging](../app/logging.md#settings). |
| `log.folder_budget` | `i64` | `0` | init only | Bytes of old log files to keep. 0 keeps all. 0 to 9007199254740992. |
| `log.pattern` | `string` | `"%^%Y-%m-%d %H:%M:%S.%e %Q %-10n %j %v%$"` | init only | Layout of each line, an spdlog pattern. `%Q` is the level letter and `%j` the thread. |
| `log.color` | `bool` | `false` | init only | Color the terminal output. |

### perf

Every program has these settings. Declared by `PerfSettings` in `rexbase`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `perf.profiler` | `bool` | `false` | init only | Start Tracy at startup. Needs a build with Tracy. |
| `perf.alloc_stack_depth` | `i32` | `0` |  | Stack frames Tracy captures per allocation. 0 to 32. |

### apu

Every program has these settings. Declared by [`AudioSettings`](../devices/audio.md).

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `apu.driver` | `string` | `"sdl3"` | init only | Audio driver, `sdl3` or `none`. The driver loads only for an app that requires audio. `none` plays no sound and fails in an app that requires audio, such as a `TitleApp`. |
| `apu.mute` | `bool` | `false` |  | Mute. |
| `apu.mute_unfocused` | `bool` | `false` |  | Mute while the window has no focus. |
| `apu.gain` | `f64` | `1.0` |  | Output gain. 0.0 to 4.0. |
| `apu.center` | `f64` | `1.0` |  | Center level on a surround device. 0.0 to 2.0. |
| `apu.surround` | `f64` | `1.0` |  | Surround level on a surround device. 0.0 to 2.0. |
| `apu.lfe` | `f64` | `1.0` |  | LFE level on a surround device. 0.0 to 2.0. |
| `apu.peak_limit` | `f64` | `0.0` |  | Silences a stream whose peak stays above it. 0 is off. 0.0 to 8.0. |
| `apu.max_queued_frames` | `u32` | `8` | locked in game | Frames one stream may queue. 4 to 64. |

### hid

Every program has these settings. Declared by [`InputSettings`](../devices/input.md).

- A keybind is a comma separated list of key chords such as `Shift+Up`, used while `hid.mnk_mode` is on.
- The settings overlay captures a keybind value from the keyboard.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `hid.drivers` | `string` | `"sdl3"` | init only | The input drivers to start, as names separated by commas, such as `sdl3,xinput`. Every listed driver runs. When two drivers see the same pad, the driver listed first wins. |
| `hid.shared_pads` | `bool` | `false` | init only | Every input device goes to user 0. |
| `hid.vibration` | `bool` | `true` |  | Allow vibration. |
| `hid.guide_button` | `bool` | `false` |  | Pass the guide button to the game. |
| `hid.left_stick_deadzone` | `f64` | `0.0` |  | Stick movement inside this fraction of full travel is ignored. 0.0 to 0.99. |
| `hid.right_stick_deadzone` | `f64` | `0.0` |  | Stick movement inside this fraction of full travel is ignored. 0.0 to 0.99. |
| `hid.mnk_mode` | `bool` | `false` |  | Keyboard and mouse act as a gamepad. |
| `hid.mnk_mouse` | `bool` | `false` |  | The mouse moves the right stick. |
| `hid.mnk_sensitivity` | `f64` | `1.0` |  | Mouse to stick scale. 0.01 to 100.0. |
| `hid.mnk_passthrough` | `bool` | `false` |  | Keys reach the game as keystrokes. |
| `hid.keybind_a` | `string` | `"Semicolon,Space"` |  | A button. |
| `hid.keybind_b` | `string` | `"Quote,Backspace"` |  | B button. |
| `hid.keybind_x` | `string` | `"L"` |  | X button. |
| `hid.keybind_y` | `string` | `"P"` |  | Y button. |
| `hid.keybind_left_trigger` | `string` | `"Q,I"` |  | Left trigger. |
| `hid.keybind_right_trigger` | `string` | `"E,O"` |  | Right trigger. |
| `hid.keybind_left_shoulder` | `string` | `"1"` |  | Left shoulder. |
| `hid.keybind_right_shoulder` | `string` | `"3"` |  | Right shoulder. |
| `hid.keybind_lstick_up` | `string` | `"W"` |  | Left stick up. |
| `hid.keybind_lstick_down` | `string` | `"S"` |  | Left stick down. |
| `hid.keybind_lstick_left` | `string` | `"A"` |  | Left stick left. |
| `hid.keybind_lstick_right` | `string` | `"D"` |  | Left stick right. |
| `hid.keybind_lstick_press` | `string` | `"F"` |  | Left stick press. |
| `hid.keybind_rstick_up` | `string` | `"Up"` |  | Right stick up. |
| `hid.keybind_rstick_down` | `string` | `"Down"` |  | Right stick down. |
| `hid.keybind_rstick_left` | `string` | `"Left"` |  | Right stick left. |
| `hid.keybind_rstick_right` | `string` | `"Right"` |  | Right stick right. |
| `hid.keybind_rstick_press` | `string` | `"K"` |  | Right stick press. |
| `hid.keybind_dpad_up` | `string` | `"Shift+Up"` |  | D pad up. |
| `hid.keybind_dpad_down` | `string` | `"Shift+Down"` |  | D pad down. |
| `hid.keybind_dpad_left` | `string` | `"Shift+Left"` |  | D pad left. |
| `hid.keybind_dpad_right` | `string` | `"Shift+Right"` |  | D pad right. |
| `hid.keybind_back` | `string` | `"Z,Tab"` |  | Back button. |
| `hid.keybind_start` | `string` | `"X,Return"` |  | Start button. |
| `hid.keybind_guide` | `string` | `""` |  | Guide button. |

### hid.sdl

This setting exists while the SDL3 input driver is selected. Declared by `SDL3InputDriver` in `rexhid-sdl3`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `hid.sdl.mappings_file` | `path` | `"{app.root}/gamecontrollerdb.txt"` | init only | SDL gamepad mappings file. |

### gpu

Every program has these settings. Declared by [`GraphicsSettings`](../devices/graphics.md).

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `gpu.driver` | `string` | `"any"` | init only | `d3d12`, `vulkan`, `null`, `none` or `any`. `any` is `d3d12` on Windows and `vulkan` elsewhere. `null` runs the graphics commands of the game and shows no picture. `none` fails in an app that requires graphics, such as a `TitleApp`. |
| `gpu.vsync` | `bool` | `true` |  | Wait for vblank when presenting. |
| `gpu.frame_limit` | `f64` | `0.0` |  | Most presents per second. 0 is no limit. 0.0 to 1000.0. |
| `gpu.resolution_scale_x` | `i32` | `1` | requires restart | Horizontal draw resolution scale. 1 to 8. |
| `gpu.resolution_scale_y` | `i32` | `1` | requires restart | Vertical draw resolution scale. 1 to 8. |
| `gpu.anisotropic_override` | `i32` | `3` |  | -1 keeps the choice of the game, 0 is off, 1 to 5 select 1x to 16x. |

### gpu.xenos

These settings exist while a Xenos driver is initialized, with any host API. Declared by `XenosSettings`, a base class of `XenosGraphicsDriverBase`. Most of them are compatibility and debugging switches of the GPU emulation. Leave them at the default unless a game shows a rendering fault.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `gpu.xenos.present_letterbox` | `bool` | `true` |  | Keep the aspect ratio of the game with bars. |
| `gpu.xenos.present_safe_area_x` | `i32` | `90` |  | The horizontal safe area as a percent of the screen. 50 to 100. |
| `gpu.xenos.present_safe_area_y` | `i32` | `90` |  | The vertical safe area as a percent of the screen. 50 to 100. |
| `gpu.xenos.present_dither` | `bool` | `false` |  | Dither when presenting to an 8 bit swap chain. |
| `gpu.xenos.present_allow_overscan_cutoff` | `bool` | `false` |  | Allow the presented image to cut off the overscan area. |
| `gpu.xenos.readback_resolve` | `string` | `"none"` |  | Copies the result of each render to texture resolve back to guest memory, for games that read it. `none` is off. `fast` and `some` read the result one frame late without waiting for the GPU, and `some` copies less often. `full` waits for the GPU and copies every resolve, which is slow. A value set here, even `none`, overrides `gpu.d3d12.readback_resolve` and `gpu.vulkan.readback_resolve`. |
| `gpu.xenos.readback_memexport` | `bool` | `true` |  | Copies data that shaders write with memory export back to guest memory, which keeps guest memory coherent and costs GPU and CPU synchronization. `gpu.d3d12.readback_memexport` and `gpu.vulkan.readback_memexport` override it once any source sets them. |
| `gpu.xenos.query_occlusion_fake_sample_count` | `i32` | `1000` |  | The sample count that the game receives for an occlusion query that the host does not run, such as while `gpu.xenos.occlusion_query_enable` is off. 1 to 100000. |
| `gpu.xenos.allow_invalid_fetch_constants` | `bool` | `false` |  | Use a texture fetch constant of the invalid type. When off, the fetch is skipped with a warning. |
| `gpu.xenos.dump_shaders` | `string` | `""` |  | Shader dump folder. Empty dumps nothing. |
| `gpu.xenos.swap_post_effect` | `string` | `"none"` | requires restart | Antialiasing filter applied to the final image. `none`, `fxaa` or `fxaa_extreme`. |
| `gpu.xenos.store_shaders` | `bool` | `true` |  | Cache compiled shaders on disk. |
| `gpu.xenos.mrt_edram_used_range_clamp_to_min` | `bool` | `true` |  | Clamp the used EDRAM range of multiple render targets to the minimum. |
| `gpu.xenos.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend` | `bool` | `true` |  | Run the vertex shader of an unclipped draw on the CPU when render targets use pixel shader interlock, which is the `rov` path on Direct3D 12 and the `fsi` path on Vulkan. |
| `gpu.xenos.texture_cache_memory_limit_render_to_texture` | `i32` | `24` | requires restart | The texture cache memory limit for render to texture, in MB. 1 to 256. |
| `gpu.xenos.texture_cache_memory_limit_soft` | `i32` | `384` | requires restart | The soft texture cache memory limit in MB. 64 to 4096. |
| `gpu.xenos.texture_cache_memory_limit_hard` | `i32` | `768` | requires restart | The hard texture cache memory limit in MB. 128 to 8192. |
| `gpu.xenos.texture_cache_memory_limit_soft_lifetime` | `i32` | `30` |  | How many seconds a texture may exceed the soft limit before it is removed. 1 to 3600. |
| `gpu.xenos.pre_mask_resolve_l2_block` | `bool` | `true` |  | Mask scaled resolve L2 blocks to the write range before iterating. |
| `gpu.xenos.force_convert_line_loops_to_strips` | `bool` | `false` |  | Convert line loops to line strips. |
| `gpu.xenos.force_convert_quad_lists_to_triangle_lists` | `bool` | `false` |  | Convert quad lists to triangle lists. |
| `gpu.xenos.force_convert_triangle_fans_to_lists` | `bool` | `false` |  | Convert triangle fans to triangle lists. |
| `gpu.xenos.primitive_processor_cache_min_indices` | `i32` | `0` |  | The smallest index count for which the primitive processor caches a result. 0 to 1000000. |
| `gpu.xenos.half_pixel_offset` | `bool` | `true` |  | Apply the half pixel offset. |
| `gpu.xenos.resolve_resolution_scale_fill_half_pixel_offset` | `bool` | `true` |  | Fill the half pixel offset during a resolve with resolution scaling. |
| `gpu.xenos.execute_unclipped_draw_vs_on_cpu` | `bool` | `false` |  | Run the vertex shader of an unclipped draw on the CPU. |
| `gpu.xenos.execute_unclipped_draw_vs_on_cpu_with_scissor` | `bool` | `false` |  | Run the vertex shader of an unclipped draw on the CPU, with the scissor applied. |
| `gpu.xenos.clear_memory_page_state` | `bool` | `true` |  | Refresh page valid state from GPU written memory each frame. Off can break coherency. |
| `gpu.xenos.occlusion_query_enable` | `bool` | `true` |  | Handle occlusion queries on the host. |
| `gpu.xenos.readback_resolve_half_pixel_offset` | `bool` | `false` |  | With draw resolution scaling active, sample from the center of each scaled block when a resolve readback scales down. |
| `gpu.xenos.readback_memexport_fast` | `bool` | `true` |  | Use the fast double buffered memexport readback when possible. It falls back to the full synchronous readback. |
| `gpu.xenos.async_shader_compilation` | `bool` | `true` |  | Compile shaders and pipelines on background threads. Brief artifacts can appear until compilation finishes. |
| `gpu.xenos.native_2x_msaa` | `bool` | `true` |  | Use native 2x MSAA. |
| `gpu.xenos.depth_float24_round` | `bool` | `false` |  | Round float24 depth values. |
| `gpu.xenos.depth_float24_convert_in_pixel_shader` | `bool` | `false` |  | Convert float24 depth in the pixel shader. |
| `gpu.xenos.depth_transfer_not_equal_test` | `bool` | `true` |  | Use the not equal test for depth transfer. |
| `gpu.xenos.gamma_render_target_as_unorm16` | `bool` | `true` |  | Use R16G16B16A16_UNORM for gamma render targets. |
| `gpu.xenos.snorm16_render_target_full_range` | `bool` | `true` |  | Use the full range for SNORM16 render targets. |
| `gpu.xenos.direct_host_resolve` | `bool` | `true` |  | Resolve from host render targets to shared memory when possible. |
| `gpu.xenos.texture_3d_to_2d` | `bool` | `true` |  | Sample 3D textures as 2D textures. |

### gpu.vulkan

These settings exist while the Vulkan driver is initialized. Declared by `VulkanSettings`, a member of `VulkanXenosGraphicsDriver`. The two `require_` settings default to `false` on macOS and `true` elsewhere. Most of these settings are compatibility and debugging switches. Leave them at the default unless a game shows a rendering fault.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `gpu.vulkan.device` | `i32` | `-1` | init only | Physical device index. -1 is automatic. -1 to 15. |
| `gpu.vulkan.readback_resolve` | `bool` | `false` |  | True copies render to texture results back to guest memory as `fast` does for `gpu.xenos.readback_resolve`. Used only while `gpu.xenos.readback_resolve` is `none` and no source set it. |
| `gpu.vulkan.readback_memexport` | `bool` | `false` |  | Copies data that shaders write with memory export back to guest memory. Overrides `gpu.xenos.readback_memexport` once any source sets it. |
| `gpu.vulkan.async_skip_incomplete_frames` | `bool` | `true` |  | With async shader compilation, skip frames that used placeholder pipelines. |
| `gpu.vulkan.submit_on_primary_buffer_end` | `bool` | `true` |  | Submit the command buffer when a PM4 primary buffer ends. |
| `gpu.vulkan.dynamic_rendering` | `bool` | `true` |  | Use VK_KHR_dynamic_rendering when supported. |
| `gpu.vulkan.pipeline_creation_threads` | `i32` | `-1` | requires restart | Pipeline creation threads. -1 is automatic. -1 to 32. |
| `gpu.vulkan.tessellation_wireframe` | `bool` | `false` |  | Render tessellation as wireframe. |
| `gpu.vulkan.force_expand_point_sprites_in_vs` | `bool` | `false` | init only | Expand point sprites in the vertex shader even when geometry shaders are available. |
| `gpu.vulkan.force_expand_rectangle_lists_in_vs` | `bool` | `false` | init only | Expand rectangle lists in the vertex shader even when geometry shaders are available. |
| `gpu.vulkan.force_convert_quad_lists_to_triangle_lists` | `bool` | `false` | init only | Convert quad lists to triangle lists in primitive processing even when geometry shaders are available. |
| `gpu.vulkan.render_target_path` | `string` | `""` | init only | How render targets are emulated, not a folder. `fsi` uses fragment shader interlock, and `fbo` or empty uses framebuffer objects. Any other value is refused. |
| `gpu.vulkan.sparse_shared_memory` | `bool` | `true` | requires restart | Use sparse shared memory. |
| `gpu.vulkan.cube_map_edge_filtering_off` | `bool` | `false` |  | Sample cube maps without filtering across the edges of faces. |
| `gpu.vulkan.require_geometry_shader` | `bool` | `true` | init only | Require geometryShader support. Turn it off to allow the fallback primitive emulation paths. |
| `gpu.vulkan.require_fill_mode_non_solid` | `bool` | `true` | init only | Require fillModeNonSolid support. Turn it off to fall back to solid fill for line and point polygon modes. |
| `gpu.vulkan.log_debug_messages` | `bool` | `true` |  | Log Vulkan debug messages. |
| `gpu.vulkan.render_pass_clear` | `bool` | `true` |  | Clear the render pass during presentation. |
| `gpu.vulkan.allow_present_mode_immediate` | `bool` | `true` |  | Allow the immediate present mode, which does not wait for vsync. |
| `gpu.vulkan.allow_present_mode_mailbox` | `bool` | `true` |  | Allow the mailbox present mode, which gives triple buffering. |
| `gpu.vulkan.allow_present_mode_fifo_relaxed` | `bool` | `true` |  | Allow the FIFO relaxed present mode. |
| `gpu.vulkan.validation` | `bool` | `false` | init only | Enable the Vulkan validation layers. |
| `gpu.vulkan.prefer_geometry_shader` | `bool` | `true` | init only | When selecting a device automatically, prefer devices that support geometryShader. |
| `gpu.vulkan.prefer_fragment_stores_and_atomics` | `bool` | `true` | init only | When selecting a device automatically, prefer devices that support fragmentStoresAndAtomics. |
| `gpu.vulkan.prefer_vertex_pipeline_stores_and_atomics` | `bool` | `true` | init only | When selecting a device automatically, prefer devices that support vertexPipelineStoresAndAtomics. |
| `gpu.vulkan.prefer_fill_mode_non_solid` | `bool` | `true` | init only | When selecting a device automatically, prefer devices that support fillModeNonSolid. |

### gpu.d3d12

These settings exist while the Direct3D 12 driver is initialized, on Windows only. Declared by `D3D12Settings`, a member of `D3D12XenosGraphicsDriver`. Most of these settings are compatibility and debugging switches. Leave them at the default unless a game shows a rendering fault.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `gpu.d3d12.adapter` | `i32` | `-1` | init only | Adapter index. -1 is the first hardware adapter, -2 is WARP. -2 to 15. |
| `gpu.d3d12.debug_markers` | `bool` | `false` |  | Debug markers for PIX and RenderDoc. Turns on when RenderDoc is detected. |
| `gpu.d3d12.bindless` | `bool` | `true` | requires restart | Use bindless resources where available. |
| `gpu.d3d12.readback_memexport` | `bool` | `false` |  | Copies data that shaders write with memory export back to guest memory. Overrides `gpu.xenos.readback_memexport` once any source sets it. |
| `gpu.d3d12.readback_resolve` | `bool` | `false` |  | True copies render to texture results back to guest memory as `fast` does for `gpu.xenos.readback_resolve`. Used only while `gpu.xenos.readback_resolve` is `none` and no source set it. |
| `gpu.d3d12.submit_on_primary_buffer_end` | `bool` | `true` |  | Submit the command list when a PM4 primary buffer ends. |
| `gpu.d3d12.dxbc_disasm` | `bool` | `false` |  | Dump the DXBC disassembly of shaders. |
| `gpu.d3d12.dxbc_disasm_dxilconv` | `bool` | `false` |  | Dump the disassembly of the DXIL conversion. |
| `gpu.d3d12.pipeline_creation_threads` | `i32` | `-1` | requires restart | Pipeline creation threads. -1 is automatic. -1 to 32. |
| `gpu.d3d12.tessellation_wireframe` | `bool` | `false` |  | Render tessellation as wireframe. |
| `gpu.d3d12.native_stencil_value_output` | `bool` | `true` |  | Use native stencil value output. |
| `gpu.d3d12.use_fuzzy_alpha_epsilon` | `bool` | `false` |  | Compare alpha test values approximately, which prevents flickering on NVIDIA graphics cards. |
| `gpu.d3d12.dxbc_switch` | `bool` | `true` |  | Use switch statements in DXBC. |
| `gpu.d3d12.dxbc_source_map` | `bool` | `false` |  | Generate source maps for DXBC. |
| `gpu.d3d12.draw_resolution_scaled_texture_offsets` | `bool` | `true` |  | Scale texture offsets with the draw resolution. |
| `gpu.d3d12.native_stencil_value_output_intel` | `bool` | `false` |  | Use native stencil value output on Intel graphics. |
| `gpu.d3d12.render_target_path` | `string` | `""` | init only | How render targets are emulated, not a folder. `rtv` uses render target views and `rov` uses rasterizer ordered views. Empty uses rasterizer ordered views on an Intel adapter that supports them, and render target views on any other adapter. Any other value is refused. |
| `gpu.d3d12.tiled_shared_memory` | `bool` | `true` | requires restart | Use tiled shared memory. |
| `gpu.d3d12.allow_tearing` | `bool` | `true` |  | Allow variable refresh rate and tearing. |
| `gpu.d3d12.debug` | `bool` | `false` | init only | Enable the Direct3D 12 and DXGI debug layer. |
| `gpu.d3d12.break_on_error` | `bool` | `false` |  | Break into the debugger on a Direct3D 12 validation error. |
| `gpu.d3d12.break_on_warning` | `bool` | `false` |  | Break into the debugger on a Direct3D 12 validation warning. |
| `gpu.d3d12.queue_priority` | `i32` | `1` |  | Graphics queue priority. 0 normal, 1 high, 2 realtime. |

### runtime

These settings exist while the `rexkernel` module is connected. Declared by `RuntimeSettings`, which `rexkernel` adds to the registry in `Connect`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `runtime.heap_size_mb` | `u32` | `256` | locked in game | Size in MiB of the first segment of the heap that the [shims](../runtime/shims.md) allocate from. 4 to 2048. |
| `runtime.title_memory_mb` | `u32` | `480` | locked in game | Title memory cap in MiB. 0 removes the cap. 0 to 512. |
| `runtime.writable_code_segments` | `bool` | `false` | locked in game | Load code and read only data as writable memory. |
| `runtime.protect_zero` | `bool` | `true` | locked in game | Any access to the first 64 KiB of guest memory faults, which catches null pointers. |
| `runtime.protect_on_release` | `bool` | `false` | locked in game | Released memory faults on access. |
| `runtime.scribble_heap` | `bool` | `false` | locked in game | Fill new heap memory with a pattern. |
| `runtime.ignore_thread_priorities` | `bool` | `true` |  | Ignore the priorities that guest threads set. |
| `runtime.ignore_thread_affinities` | `bool` | `true` |  | Ignore the affinities that guest threads set. |
| `runtime.ffmpeg_verbose` | `bool` | `false` |  | Log FFmpeg messages during XMA decoding. |
| `runtime.terminate_timeout_ms` | `u32` | `2000` |  | How long `Unload` waits for guest threads, in ms. 0 to 60000. |

### kernel

These settings exist while the `rexkernel` module is connected. Declared by `KernelSettings`, which `rexkernel` adds to the registry in `Connect`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `kernel.headless` | `bool` | `false` |  | System dialogs return immediately without showing. |
| `kernel.user_language` | `u32` | `1` | locked in game | Language that the game reads, as an XLanguage ID. 0 follows the host language. |
| `kernel.user_country` | `u32` | `103` | locked in game | Country code that the game reads from the console configuration. |
| `kernel.license_mask` | `u32` | `0` | locked in game | The license bits that `XamContentGetLicenseMask` returns to the game, combined with OR. |
| `kernel.command_line` | `string` | `""` | locked in game | Command line arguments that the game sees after `default.xex`. |

### title

These settings exist while the `rexkernel` module is connected. Declared by `TitlePaths`, which `rexkernel` adds to the registry in `Connect`. The [command line options](#command-line-options) `--game_data_root`, `--update_data_root` and `--metadata_root` set the three folders too.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `title.game_root` | `path` | `""` | locked in game | Game files, mounted read only as `game:`. |
| `title.game_writable` | `bool` | `false` | locked in game | Mount `game:` read write. |
| `title.update_root` | `path` | `""` | locked in game | Title update files. Empty means none. |
| `title.metadata_root` | `path` | `""` | locked in game | Achievement icons and other metadata. Empty searches near `title.game_root`. |
| `title.entry` | `string` | `"default.xex"` | locked in game | Executable under `game:` that `TitleRuntime::Load` loads. Empty loads none. |
| `title.imports` | `string` | `""` | locked in game | Files under `game:` the executable may import from, separated by `\|`. |

### ui

Every program has these settings. Declared by [`UISettings`](../devices/ui.md).

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `ui.driver` | `string` | `"sdl3"` | init only | UI driver, `sdl3` or `none`. The driver loads only for an app that requires a UI. `none` runs headless and fails in an app that requires a UI, such as a `TitleApp`. |
| `ui.title` | `string` | `""` |  | Empty uses the program name and the build label. |
| `ui.window_width` | `i32` | `0` |  | 0 uses the video mode width. 0 to 16384. |
| `ui.window_height` | `i32` | `0` |  | 0 uses the video mode height. 0 to 16384. |
| `ui.fullscreen` | `bool` | `true` |  | Fullscreen. |
| `ui.fullscreen_exclusive` | `bool` | `false` |  | Exclusive instead of borderless fullscreen. |
| `ui.monitor` | `i32` | `0` |  | Display index. 0 to 16. |
| `ui.min_width` | `i32` | `0` |  | Smallest window width. 0 to 16384. |
| `ui.min_height` | `i32` | `0` |  | Smallest window height. 0 to 16384. |
| `ui.aspect_ratio` | `string` | `"stretch"` |  | Shape that the window keeps when resized. `stretch` keeps no fixed shape, and the other values are the ratios `4:3`, `5:4`, `3:2`, `16:10`, `16:9`, `21:9` or `32:9`. |
| `ui.resolution` | `string` | `""` |  | One value for both the window size and the video mode, which is the display mode that the console reports to the game. A preset such as `720p`, `1080p` or `4k`, or `WxH` such as `1920x1080`. Empty uses the `ui.window_*` and `ui.video_mode_*` settings. |
| `ui.video_mode_width` | `i32` | `1280` | requires restart | Width of the video mode that the console reports to the game. 640 to 4095. |
| `ui.video_mode_height` | `i32` | `720` | requires restart | Height of the video mode that the console reports to the game. 480 to 4095. |
| `ui.video_mode_refresh_rate` | `f64` | `60.0` | requires restart | Refresh rate of the video mode that the console reports to the game. 24.0 to 240.0. |
| `ui.cursor_timeout` | `f64` | `0.0` |  | Seconds before an idle cursor hides. 0 never. 0.0 to 3600.0. |

### ui.sdl

This setting exists while the SDL3 UI driver is selected. Declared by `SDL3UIDriver` in `rexui-sdl3`.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `ui.sdl.video_driver` | `string` | `""` | init only | Empty uses the SDL default. `dummy` runs with no display. |

### log.categories

Each log category adds a string setting named after it, such as `log.categories.gpu` for the `gpu` category, that sets the level of that category. Declared by `Logger`. See [Log Categories](../appendix/log-categories-and-macros.md#built-in-categories) for the categories of the SDK.

- The default is empty, which uses `log.level`.
- Allowed values are `trace`, `dev`, `debug`, `info`, `warn`, `error`, `critical` and `off`.
- The categories depend on the loaded modules.

### ui.bind

Each overlay with a default key gets a hotkey setting, which the UI registers while the window is open.

- Each value is a key chord such as `Ctrl+K`.
- An empty value unbinds the key.

These are the four standard overlays.

| Name | Type | Default | Restriction | Meaning |
|---|---|---|---|---|
| `ui.bind.debug` | `string` | `"F3"` |  | Toggles the debug overlay. |
| `ui.bind.console` | `string` | `"Backtick"` |  | Toggles the console overlay. |
| `ui.bind.settings` | `string` | `"F4"` |  | Toggles the settings overlay. |
| `ui.bind.achievements` | `string` | `"F7"` |  | Toggles the achievements overlay. |

### console

Console commands run from the console overlay. They have dotted names like settings, but hold no value. `help` and `clear` are built into the overlay and are not registered commands. Declared by `ConsoleCommandTable`, which the UI adds to the registry while the window is open.

| Name | Meaning |
|---|---|
| `console.echo` | Writes its arguments to the log. |
| `console.find` | Writes the setting and command names that contain a substring to the log. |

## Old Names

- Each old name sets its new setting from the config file, the command line and `SettingsRegistry::SetText`. The environment is read only under the new names.
- Each use of an old name logs a warning.

| Old name | Setting |
|---|---|
| `xenos.readback_resolve` | `gpu.xenos.readback_resolve` |
| `xenos.readback_memexport` | `gpu.xenos.readback_memexport` |
| `xenos.query_occlusion_fake_sample_count` | `gpu.xenos.query_occlusion_fake_sample_count` |
| `xenos.allow_invalid_fetch_constants` | `gpu.xenos.allow_invalid_fetch_constants` |
| `xenos.dump_shaders` | `gpu.xenos.dump_shaders` |
| `xenos.swap_post_effect` | `gpu.xenos.swap_post_effect` |
| `xenos.store_shaders` | `gpu.xenos.store_shaders` |
| `xenos.mrt_edram_used_range_clamp_to_min` | `gpu.xenos.mrt_edram_used_range_clamp_to_min` |
| `xenos.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend` | `gpu.xenos.execute_unclipped_draw_vs_on_cpu_for_psi_render_backend` |
| `xenos.texture_cache_memory_limit_render_to_texture` | `gpu.xenos.texture_cache_memory_limit_render_to_texture` |
| `xenos.texture_cache_memory_limit_soft` | `gpu.xenos.texture_cache_memory_limit_soft` |
| `xenos.texture_cache_memory_limit_hard` | `gpu.xenos.texture_cache_memory_limit_hard` |
| `xenos.texture_cache_memory_limit_soft_lifetime` | `gpu.xenos.texture_cache_memory_limit_soft_lifetime` |
| `xenos.pre_mask_resolve_l2_block` | `gpu.xenos.pre_mask_resolve_l2_block` |
| `xenos.force_convert_line_loops_to_strips` | `gpu.xenos.force_convert_line_loops_to_strips` |
| `xenos.force_convert_quad_lists_to_triangle_lists` | `gpu.xenos.force_convert_quad_lists_to_triangle_lists` |
| `xenos.force_convert_triangle_fans_to_lists` | `gpu.xenos.force_convert_triangle_fans_to_lists` |
| `xenos.primitive_processor_cache_min_indices` | `gpu.xenos.primitive_processor_cache_min_indices` |
| `xenos.half_pixel_offset` | `gpu.xenos.half_pixel_offset` |
| `xenos.resolve_resolution_scale_fill_half_pixel_offset` | `gpu.xenos.resolve_resolution_scale_fill_half_pixel_offset` |
| `xenos.execute_unclipped_draw_vs_on_cpu` | `gpu.xenos.execute_unclipped_draw_vs_on_cpu` |
| `xenos.execute_unclipped_draw_vs_on_cpu_with_scissor` | `gpu.xenos.execute_unclipped_draw_vs_on_cpu_with_scissor` |
| `kXenosCvars.readback_resolve` | `gpu.xenos.readback_resolve` |
| `gpu.present_letterbox` | `gpu.xenos.present_letterbox` |
| `gpu.present_safe_area_x` | `gpu.xenos.present_safe_area_x` |
| `gpu.present_safe_area_y` | `gpu.xenos.present_safe_area_y` |
| `gpu.present_dither` | `gpu.xenos.present_dither` |
| `gpu.present_allow_overscan_cutoff` | `gpu.xenos.present_allow_overscan_cutoff` |
| `xenos.clear_memory_page_state` | `gpu.xenos.clear_memory_page_state` |
| `xenos.occlusion_query_enable` | `gpu.xenos.occlusion_query_enable` |
| `xenos.readback_resolve_half_pixel_offset` | `gpu.xenos.readback_resolve_half_pixel_offset` |
| `xenos.readback_memexport_fast` | `gpu.xenos.readback_memexport_fast` |
| `xenos.async_shader_compilation` | `gpu.xenos.async_shader_compilation` |
| `xenos.native_2x_msaa` | `gpu.xenos.native_2x_msaa` |
| `xenos.depth_float24_round` | `gpu.xenos.depth_float24_round` |
| `xenos.depth_float24_convert_in_pixel_shader` | `gpu.xenos.depth_float24_convert_in_pixel_shader` |
| `xenos.depth_transfer_not_equal_test` | `gpu.xenos.depth_transfer_not_equal_test` |
| `xenos.gamma_render_target_as_unorm16` | `gpu.xenos.gamma_render_target_as_unorm16` |
| `xenos.snorm16_render_target_full_range` | `gpu.xenos.snorm16_render_target_full_range` |
| `xenos.direct_host_resolve` | `gpu.xenos.direct_host_resolve` |
| `xenos.texture_3d_to_2d` | `gpu.xenos.texture_3d_to_2d` |
| `d3d12.allow_tearing` | `gpu.d3d12.allow_tearing` |
| `d3d12.debug` | `gpu.d3d12.debug` |
| `d3d12.break_on_error` | `gpu.d3d12.break_on_error` |
| `d3d12.break_on_warning` | `gpu.d3d12.break_on_warning` |
| `d3d12.queue_priority` | `gpu.d3d12.queue_priority` |
| `gpu.debug_markers` | `gpu.d3d12.debug_markers` |
| `gpu.d3d12_adapter` | `gpu.d3d12.adapter` |
| `gpu.vulkan_device` | `gpu.vulkan.device` |
| `bind.debug` | `ui.bind.debug` |
| `bind.console` | `ui.bind.console` |
| `bind.settings` | `ui.bind.settings` |
| `bind.achievements` | `ui.bind.achievements` |
| `xenos.d3d12_bindless` | `gpu.d3d12.bindless` |
| `xenos.d3d12_readback_memexport` | `gpu.d3d12.readback_memexport` |
| `xenos.d3d12_readback_resolve` | `gpu.d3d12.readback_resolve` |
| `xenos.d3d12_submit_on_primary_buffer_end` | `gpu.d3d12.submit_on_primary_buffer_end` |
| `xenos.d3d12_dxbc_disasm` | `gpu.d3d12.dxbc_disasm` |
| `xenos.d3d12_dxbc_disasm_dxilconv` | `gpu.d3d12.dxbc_disasm_dxilconv` |
| `xenos.d3d12_pipeline_creation_threads` | `gpu.d3d12.pipeline_creation_threads` |
| `xenos.d3d12_tessellation_wireframe` | `gpu.d3d12.tessellation_wireframe` |
| `xenos.d3d12_native_stencil_value_output_intel` | `gpu.d3d12.native_stencil_value_output_intel` |
| `xenos.d3d12_render_target_path` | `gpu.d3d12.render_target_path` |
| `xenos.d3d12_tiled_shared_memory` | `gpu.d3d12.tiled_shared_memory` |
| `xenos.native_stencil_value_output` | `gpu.d3d12.native_stencil_value_output` |
| `xenos.use_fuzzy_alpha_epsilon` | `gpu.d3d12.use_fuzzy_alpha_epsilon` |
| `xenos.dxbc_switch` | `gpu.d3d12.dxbc_switch` |
| `xenos.dxbc_source_map` | `gpu.d3d12.dxbc_source_map` |
| `xenos.draw_resolution_scaled_texture_offsets` | `gpu.d3d12.draw_resolution_scaled_texture_offsets` |
| `vulkan.require_geometry_shader` | `gpu.vulkan.require_geometry_shader` |
| `vulkan.require_fill_mode_non_solid` | `gpu.vulkan.require_fill_mode_non_solid` |
| `vulkan.log_debug_messages` | `gpu.vulkan.log_debug_messages` |
| `vulkan.render_pass_clear` | `gpu.vulkan.render_pass_clear` |
| `vulkan.allow_present_mode_immediate` | `gpu.vulkan.allow_present_mode_immediate` |
| `vulkan.allow_present_mode_mailbox` | `gpu.vulkan.allow_present_mode_mailbox` |
| `vulkan.allow_present_mode_fifo_relaxed` | `gpu.vulkan.allow_present_mode_fifo_relaxed` |
| `vulkan.validation` | `gpu.vulkan.validation` |
| `vulkan.prefer_geometry_shader` | `gpu.vulkan.prefer_geometry_shader` |
| `vulkan.prefer_fragment_stores_and_atomics` | `gpu.vulkan.prefer_fragment_stores_and_atomics` |
| `vulkan.prefer_vertex_pipeline_stores_and_atomics` | `gpu.vulkan.prefer_vertex_pipeline_stores_and_atomics` |
| `vulkan.prefer_fill_mode_non_solid` | `gpu.vulkan.prefer_fill_mode_non_solid` |
| `xenos.vulkan_readback_resolve` | `gpu.vulkan.readback_resolve` |
| `xenos.vulkan_readback_memexport` | `gpu.vulkan.readback_memexport` |
| `xenos.vulkan_async_skip_incomplete_frames` | `gpu.vulkan.async_skip_incomplete_frames` |
| `xenos.vulkan_submit_on_primary_buffer_end` | `gpu.vulkan.submit_on_primary_buffer_end` |
| `xenos.vulkan_dynamic_rendering` | `gpu.vulkan.dynamic_rendering` |
| `xenos.vulkan_pipeline_creation_threads` | `gpu.vulkan.pipeline_creation_threads` |
| `xenos.vulkan_tessellation_wireframe` | `gpu.vulkan.tessellation_wireframe` |
| `xenos.vulkan_force_expand_point_sprites_in_vs` | `gpu.vulkan.force_expand_point_sprites_in_vs` |
| `xenos.vulkan_force_expand_rectangle_lists_in_vs` | `gpu.vulkan.force_expand_rectangle_lists_in_vs` |
| `xenos.vulkan_force_convert_quad_lists_to_triangle_lists` | `gpu.vulkan.force_convert_quad_lists_to_triangle_lists` |
| `xenos.vulkan_render_target_path` | `gpu.vulkan.render_target_path` |
| `xenos.vulkan_sparse_shared_memory` | `gpu.vulkan.sparse_shared_memory` |
| `xenos.vulkan_cube_map_edge_filtering_off` | `gpu.vulkan.cube_map_edge_filtering_off` |
| `hid.mappings_file` | `hid.sdl.mappings_file` |
| `ui.video_driver` | `ui.sdl.video_driver` |
| `paths.game_data_root` | `title.game_root` |
| `paths.user_data_root` | `app.user_root` |
| `paths.update_data_root` | `title.update_root` |
| `paths.cache_root` | `app.cache_root` |
| `paths.metadata_root` | `title.metadata_root` |
| `paths.config_file` | `app.config_file` |
| `runtime.terminate_timeout` | `runtime.terminate_timeout_ms` |
| `runtime.kernel.headless` | `kernel.headless` |
| `runtime.kernel.user_language` | `kernel.user_language` |
| `runtime.kernel.user_country` | `kernel.user_country` |
| `runtime.kernel.license_mask` | `kernel.license_mask` |
| `runtime.kernel.command_line` | `kernel.command_line` |
| `profiler` | `perf.profiler` |
| `single_instance` | `app.single_instance` |
| `audio_maxqframes` | `apu.max_queued_frames` |
| `audio_mute` | `apu.mute` |
| `ffmpeg_verbose` | `runtime.ffmpeg_verbose` |
| `clock_no_scaling` | `clock.no_scaling` |
| `log_level` | `log.level` |
| `log_file` | `log.file_name` |
| `log_flush_interval` | `log.flush_interval` |
| `guide_button` | `hid.guide_button` |
| `vibration` | `hid.vibration` |
| `left_stick_deadzone_percentage` | `hid.left_stick_deadzone` |
| `right_stick_deadzone_percentage` | `hid.right_stick_deadzone` |
| `mnk_mode` | `hid.mnk_mode` |
| `mnk_mouse` | `hid.mnk_mouse` |
| `mnk_sensitivity` | `hid.mnk_sensitivity` |
| `mnk_passthrough` | `hid.mnk_passthrough` |
| `keybind_a` | `hid.keybind_a` |
| `keybind_b` | `hid.keybind_b` |
| `keybind_x` | `hid.keybind_x` |
| `keybind_y` | `hid.keybind_y` |
| `keybind_left_trigger` | `hid.keybind_left_trigger` |
| `keybind_right_trigger` | `hid.keybind_right_trigger` |
| `keybind_left_shoulder` | `hid.keybind_left_shoulder` |
| `keybind_right_shoulder` | `hid.keybind_right_shoulder` |
| `keybind_lstick_up` | `hid.keybind_lstick_up` |
| `keybind_lstick_down` | `hid.keybind_lstick_down` |
| `keybind_lstick_left` | `hid.keybind_lstick_left` |
| `keybind_lstick_right` | `hid.keybind_lstick_right` |
| `keybind_lstick_press` | `hid.keybind_lstick_press` |
| `keybind_rstick_up` | `hid.keybind_rstick_up` |
| `keybind_rstick_down` | `hid.keybind_rstick_down` |
| `keybind_rstick_left` | `hid.keybind_rstick_left` |
| `keybind_rstick_right` | `hid.keybind_rstick_right` |
| `keybind_rstick_press` | `hid.keybind_rstick_press` |
| `keybind_dpad_up` | `hid.keybind_dpad_up` |
| `keybind_dpad_down` | `hid.keybind_dpad_down` |
| `keybind_dpad_left` | `hid.keybind_dpad_left` |
| `keybind_dpad_right` | `hid.keybind_dpad_right` |
| `keybind_back` | `hid.keybind_back` |
| `keybind_start` | `hid.keybind_start` |
| `keybind_guide` | `hid.keybind_guide` |
| `writable_code_segments` | `runtime.writable_code_segments` |
| `protect_zero` | `runtime.protect_zero` |
| `protect_on_release` | `runtime.protect_on_release` |
| `scribble_heap` | `runtime.scribble_heap` |
| `ignore_thread_priorities` | `runtime.ignore_thread_priorities` |
| `ignore_thread_affinities` | `runtime.ignore_thread_affinities` |
| `window_width` | `ui.window_width` |
| `window_height` | `ui.window_height` |
| `fullscreen` | `ui.fullscreen` |
| `fullscreen_exclusive` | `ui.fullscreen_exclusive` |
| `monitor` | `ui.monitor` |
| `video_mode_width` | `ui.video_mode_width` |
| `video_mode_height` | `ui.video_mode_height` |
| `resolution` | `ui.resolution` |
| `video_mode_refresh_rate` | `ui.video_mode_refresh_rate` |

### Removed Names

These names have no setting. The registry ignores a value set under one of them, and each use logs a warning.

| Old name | Effect |
|---|---|
| `xenos.present_from_non_ui_thread` | Ignored. Presentation no longer depends on a setting. |
| `log.timestamps` | Set `log.pattern` to change the line layout. |
| `log.show_category` | Set `log.pattern` to change the line layout. |
| `log.show_thread` | Set `log.pattern` to change the line layout. |
| `filesystem.game_mount_flags` | Set `title.game_writable` to make the game folder writable. |

### log.file

`log.file` is a flag. If an older config file sets `log.file` to a path, the registry:

- Sets `log.file` to `true` and `log.file_name` to the file name.
- Sets `app.log_root` to the folder of the path, when the path has one.
- Logs a warning.

An empty value changes nothing.

## Command Line Options

These older options have no scope. Each takes a value, as in `--game_data_root=path`, and sets the setting in the table.

| Option | Setting |
|---|---|
| `--game_data_root` | `title.game_root` |
| `--user_data_root` | `app.user_root` |
| `--update_data_root` | `title.update_root` |
| `--cache_root` | `app.cache_root` |
| `--metadata_root` | `title.metadata_root` |
| `--config_file` | `app.config_file` |
| `--modules` | None. Logs a warning, because the driver settings choose the drivers. |
