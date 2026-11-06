set(CMAKE_FOLDER "tests/unit")

add_executable(
  unit_tests
  test_memory.cpp
  crash_child_runner.cpp
  hid/xam_input_error_test.cpp
  apu/audio_system_test.cpp
  apu/sdl3_audio_test.cpp
  hid/input_system_test.cpp
  hid/sdl3_input_test.cpp
  hid/bindings_test.cpp
  hid/input_source_test.cpp
  hid/key_capture_test.cpp
  hid/key_chord_test.cpp
  hid/virtual_key_test.cpp
  kernel/heap_allocation_test.cpp
  kernel/access_violation_test.cpp
  kernel/memory_budget_test.cpp
  base/byte_swap_test.cpp
  base/mapped_memory_test.cpp
  base/file_mapping_test.cpp
  base/bit_cast_copy_test.cpp
  gpu/null_command_processor_test.cpp
  gpu/fence_endianness_test.cpp
  gpu/graphics_system_test.cpp
  gpu/packet_header_test.cpp
  gpu/texture_info_test.cpp
  gpu/texture_layout_test.cpp
  gpu/primitive_processor_simd_test.cpp
  gpu/xenos_provider_test.cpp
  gpu/imgui_overlay_test.cpp
  gpu/xenos_base_test.cpp
  gpu/headless_test.cpp
  gpu/draw_state_test.cpp
  gpu/dxbc_operand_test.cpp
  gpu/present_policy_test.cpp
  gpu/vblank_pacing_test.cpp
  gpu/shader_interpreter_test.cpp
  gpu/shader_translation_test.cpp
  gpu/tessellation_glsl_test.cpp
  kernel/be_ptr_test.cpp
  kernel/exit_title_test.cpp
  kernel/xam_input_test.cpp
  kernel/object_table_test.cpp
  kernel/shim_crt_test.cpp
  kernel/shim_debug_test.cpp
  kernel/shim_math_test.cpp
  kernel/shim_registry_test.cpp
  kernel/exports_test.cpp
  kernel/xam_forward_test.cpp
  kernel/exports_visibility_test.cpp
  kernel/kernel_fixture.cpp
  kernel/lock_order_test.cpp
  kernel/pinned_wait_test.cpp
  kernel/sync_test.cpp
  kernel/enumerate_test.cpp
  kernel/handles_test.cpp
  kernel/format_test.cpp
  kernel/rtl_string_test.cpp
  kernel/strings_test.cpp
  kernel/varargs_test.cpp
  kernel/shutdown_test.cpp
  kernel/threads_test.cpp
  kernel/files_test.cpp
  kernel/memory_test.cpp
  kernel/net_test.cpp
  kernel/crypt_bignum_test.cpp
  kernel/crypt_measured_test.cpp
  kernel/crypt_cipher_test.cpp
  kernel/crypt_hash_test.cpp
  kernel/crypt_md5_test.cpp
  kernel/crypt_parve_test.cpp
  kernel/crypt_pkcs_test.cpp
  kernel/crypt_rsa_test.cpp
  kernel/crypt_sig_test.cpp
  kernel/xam_test.cpp
  kernel/xmp_test.cpp
  kernel/identity_test.cpp
  kernel/interrupt_test.cpp
  kernel/dpc_test.cpp
  kernel/notify_test.cpp
  kernel/fp_state_test.cpp
  kernel/tls_test.cpp
  kernel/fiber_test.cpp
  kernel/workspace_test.cpp
  kernel/overlapped_test.cpp
  base/assert_test.cpp
  base/container_test.cpp
  base/core_test.cpp
  base/high_resolution_timer_test.cpp
  base/host_alloc_test.cpp
  base/counter_test.cpp
  base/host_alloc_module_test.cpp
  base/host_alloc_forms_test.cpp
  base/interface_test.cpp
  base/module_test.cpp
  base/module_group_test.cpp
  base/module_served_test.cpp
  base/logger_test.cpp
  base/math_test.cpp
  base/propagate_const_test.cpp
  base/sleep_test.cpp
  base/socket_test.cpp
  base/socket_wait_test.cpp
  base/thread_start_data_test.cpp
  base/thread_priority_test.cpp
  base/thread_time_test.cpp
  base/timer_queue_test.cpp
  base/utf8_test.cpp
  base/trim_test.cpp
  base/xex_test.cpp
  filesystem/file_system_test.cpp
  filesystem/filesystem_module_test.cpp
  filesystem/host_file_test.cpp
  filesystem/open_node_test.cpp
  filesystem/overwrite_test.cpp
  filesystem/providers_test.cpp
  filesystem/rename_test.cpp
  filesystem/wildcard_test.cpp
  app/app_test.cpp
  base/fat_timestamp_test.cpp
  base/storage_file_test.cpp
  base/host_paths_test.cpp
  base/build_stamp_test.cpp
  base/xcontent_language_test.cpp
  base/process_resources_test.cpp
  base/host_file_io_test.cpp
  app/cli_action_test.cpp
  base/cli_arg_list_test.cpp
  base/settings_test.cpp
  base/aliases_test.cpp
  base/settings_feed_test.cpp
  base/driver_select_test.cpp
  base/proc_maps_test.cpp
  base/fixed_mapping_test.cpp
  base/endian_atomic_test.cpp
  base/crash_reporter_test.cpp
  base/fiber_test.cpp
  base/bit_test.cpp
  base/hash_test.cpp
  base/init_test.cpp
  base/string_buffer_test.cpp
  base/stream_test.cpp
  base/chrono_test.cpp
  base/clock_test.cpp
  base/color_test.cpp
  base/result_macros_test.cpp
  ppc/mmio_test.cpp
  base/move_only_function_test.cpp
  base/scope_guard_test.cpp
  base/profile_macros_test.cpp
  base/relaunch_test.cpp
  base/posix_wait_test.cpp
  base/thread_redirect_test.cpp
  ppc/dot_product_test.cpp
  ppc/estimate_test.cpp
  ppc/fpscr_flush_test.cpp
  ppc/vscr_register_test.cpp
  ppc/detour_test.cpp
  ppc/hook_shapes_test.cpp
  ppc/kernel_hooks_test.cpp
  ppc/midasm_macro_test.cpp
  ppc/multiply_add_test.cpp
  ppc/pack_d3d_test.cpp
  ppc/pdata_test.cpp
  ppc/saturation_test.cpp
  ppc/arg_translator_test.cpp
  ppc/stack_test.cpp
  ppc/vector_tables_test.cpp
  ppc/vector_unaligned_write_test.cpp
  kernel/timeout_test.cpp
  kernel/guest_path_test.cpp
  kernel/map_parser_test.cpp
  kernel/guest_allocator_test.cpp
  kernel/guest_memory_test.cpp
  kernel/guest_stack_range_test.cpp
  kernel/shim_heap_test.cpp
  kernel/object_block_test.cpp
  kernel/module_object_test.cpp
  xam/achievement_manager_test.cpp
  xam/content_package_test.cpp
  kernel/xcontent_test.cpp
  kernel/xdbf_test.cpp
  kernel/xstatus_test.cpp
  kernel/xtypes_test.cpp
  kernel/function_dispatcher_test.cpp
  kernel/call_test.cpp
  xam/language_test.cpp
  kernel/thread_wait_test.cpp
  kernel/runtime_error_test.cpp
  kernel/runtime_interfaces_test.cpp
  kernel/memory_view_test.cpp
  kernel/setting_keys_test.cpp
  kernel/title_launch_test.cpp
  kernel/title_runtime_test.cpp
  kernel/guest_global_test.cpp
  kernel/apu_test.cpp
  kernel/xma_decoder_test.cpp
  kernel/title_context_test.cpp
  kernel/recompiled_module_test.cpp
  ui/bind_test.cpp
  ui/console_model_test.cpp
  ui/imgui_dialog_test.cpp
  ui/prompt_dialog_test.cpp
  ui/input_capture_test.cpp
  ui/xam_prompt_test.cpp
  ui/imgui_style_test.cpp
  ui/title_app_test.cpp
  ui/ui_event_test.cpp
  ui/sdl3_ui_test.cpp
  ui/ui_system_test.cpp
  codegen/barrier_test.cpp
  codegen/codegen_writer_test.cpp
  codegen/config_include_test.cpp
  codegen/config_keys_test.cpp
  codegen/disasm_test.cpp
  codegen/flush_mode_test.cpp
  codegen/funclet_sync_test.cpp
  codegen/gap_fill_test.cpp
  codegen/instruction_comment_test.cpp
  codegen/millicode_test.cpp
  codegen/noreturn_test.cpp
  codegen/reserved_register_test.cpp
  codegen/restore_exit_test.cpp
  codegen/stack_access_test.cpp
  codegen/stack_slot_forwarding_test.cpp
  codegen/tail_call_test.cpp
  codegen/vector_pair_write_test.cpp
  codegen/setjmp_test.cpp
  codegen/skip_lr_test.cpp
  codegen/call_usage_test.cpp
  codegen/function_scanner_test.cpp
  codegen/hook_scan_test.cpp
  codegen/manifest_test.cpp
  codegen/midasm_hook_config_test.cpp
  codegen/midasm_hook_emit_test.cpp
  codegen/opcode_test.cpp
  codegen/output_partition_test.cpp
  codegen/output_stamp_test.cpp
  codegen/project_recompiler_test.cpp
  codegen/seh_scope_test.cpp
  codegen/shim_config_test.cpp
  codegen/mmio_analysis_test.cpp
  rexglue/init_command_test.cpp
  rexglue/project_scan_test.cpp
  rexglue/test_recompiler_test.cpp
  rexglue/test_spec_test.cpp
)

target_sources(
  unit_tests
  PRIVATE ${PROJECT_SOURCE_DIR}/src/gpu/xenos/null/null_command_processor.cpp
          ${PROJECT_SOURCE_DIR}/tests/code_image.cpp
          ${PROJECT_SOURCE_DIR}/tests/object_app.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/null/null_xenos_graphics_driver.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/vulkan/tessellation_glsl.cpp
          $<TARGET_OBJECTS:rexglue-objects>
          $<TARGET_OBJECTS:rexcodegen-objects>
          $<TARGET_OBJECTS:rexkernel-objects>
          $<TARGET_OBJECTS:rexxam-objects>
          $<TARGET_OBJECTS:rexapp-objects>
          $<TARGET_OBJECTS:rexhid-objects>
          $<TARGET_OBJECTS:rexapu-objects>
          $<TARGET_OBJECTS:rexui-objects>
          $<TARGET_OBJECTS:rexfilesystem-objects>
)

# The module object files are shared with the DLLs, so they mark the symbols of the other modules as
# imported, and this link finds them local.
if (MSVC)
  target_link_options(unit_tests PRIVATE "LINKER:/IGNORE:4217")
endif ()

target_include_directories(
  unit_tests
  PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/thirdparty
          ${PROJECT_SOURCE_DIR}/thirdparty/disasm ${CMAKE_CURRENT_SOURCE_DIR}
          $<TARGET_PROPERTY:rexkernel-objects,INTERFACE_INCLUDE_DIRECTORIES>
)
target_include_directories(
  unit_tests SYSTEM
  PRIVATE $<TARGET_PROPERTY:rexcodegen-objects,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>
)
# CLI11 instantiates std::wstring_convert, which libc++ marks deprecated
target_include_directories(unit_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include)

target_link_libraries(
  unit_tests
  PRIVATE rexgpu-xenos-base
          rexppc
          excrypt
          mspack
          rexgpu
          rexbase
          simde
          disasm
          o1heap
          libavcodec
          libavutil
          SDL3::SDL3
          tomlplusplus
          xxHash::xxhash
          Catch2::Catch2WithMain
          SDL3::Headers
          Vulkan::Headers
)
target_compile_definitions(
  unit_tests PRIVATE REX_BUILDING_KERNEL REX_BUILDING_XAM REX_BUILDING_APP REX_BUILDING_HID
                     REX_BUILDING_UI REX_BUILDING_APU
)
if (WIN32)
  target_link_libraries(unit_tests PRIVATE ws2_32 winmm)
endif ()
if (APPLE)
  target_link_libraries(unit_tests PRIVATE "-framework CoreFoundation")
endif ()

target_include_directories(
  unit_tests SYSTEM PRIVATE $<TARGET_PROPERTY:SPIRV,INTERFACE_INCLUDE_DIRECTORIES>
                            $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>
)

rex_target_pch(unit_tests <rex_test.h>)

target_compile_definitions(
  unit_tests
  PRIVATE REX_TEST_KERNEL_PATH="$<TARGET_FILE:rexkernel>"
          REX_TEST_XAM_PATH="$<TARGET_FILE:rexxam>"
          REX_TEST_RECOMPILED_A_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-recompiled-a>"
          REX_TEST_RECOMPILED_B_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-recompiled-b>"
          REXGLUE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/filesystem"
)

# Links the rexkernel library, as the cross modules do, and none of the kernel objects.
add_executable(
  cross_module_tests test_memory.cpp kernel/cross_module_import_test.cpp
                     kernel/hook_override_test.cpp kernel/kernel_fixture.cpp
)
target_include_directories(
  cross_module_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests
                             ${PROJECT_SOURCE_DIR}/thirdparty ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
  cross_module_tests
  PRIVATE rexcodegen
          rexkernel
          rexapp
          rexppc
          excrypt
          simde
          rexbase
          Catch2::Catch2WithMain
)
target_compile_definitions(
  cross_module_tests
  PRIVATE REX_TEST_REAL_MODULES
          REX_TEST_CROSS_EXPORTER_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-cross-exporter>"
          REX_TEST_CROSS_IMPORTER_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-cross-importer>"
)
target_compile_options(
  cross_module_tests PRIVATE -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
                             -UREXGLUE_ENABLE_PERF_COUNTERS
)
rex_target_pch(cross_module_tests <rex/kernel/title_runtime.h> <rex/ppc/context.h>)
add_dependencies(
  cross_module_tests
  rexfilesystem
  rexapu
  rexhid
  rexui
  rexgpu
  rexxam
  rexmod-test-cross-exporter
  rexmod-test-cross-importer
)
set_target_properties(
  cross_module_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexkernel>
)
rexglue_link_host_allocator(cross_module_tests)
if (UNIX AND NOT APPLE)
  rexglue_set_rpath(cross_module_tests "$ORIGIN")
endif ()

add_executable(rex-crash-test crash_child.cpp crash_child_recompiled_code.cpp)
rexglue_link_host_allocator(rex-crash-test)
if (WIN32)
  set_source_files_properties(
    crash_child_recompiled_code.cpp PROPERTIES COMPILE_OPTIONS -fasync-exceptions
  )
else ()
  set_source_files_properties(
    crash_child_recompiled_code.cpp PROPERTIES COMPILE_OPTIONS -fasynchronous-unwind-tables
  )
endif ()
target_link_libraries(rex-crash-test PRIVATE rexkernel)
set_target_properties(
  rex-crash-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexkernel>
)
if (UNIX AND NOT APPLE)
  rexglue_set_rpath(rex-crash-test "$ORIGIN")
endif ()

add_dependencies(
  unit_tests
  rexkernel
  rexapp
  rexfilesystem
  rexapu
  rexhid
  rexui
  rexgpu
  rexxam
  rexgpu-xenos-null
  rexgpu-xenos-vulkan
  rex-sample-console
  rex-sample-title
  rex-crash-test
)
if (TARGET rexgpu-xenos-d3d12)
  add_dependencies(unit_tests rexgpu-xenos-d3d12)
endif ()
target_compile_definitions(unit_tests PRIVATE REX_GPU_XENOS_STATIC)

add_dependencies(
  unit_tests
  rexmod-test-noidentity
  rexapu-sdl3
  rexhid-sdl3
  rexui-sdl3
  rexmod-test-recompiled-a
  rexmod-test-recompiled-b
  rexmodule-test-good
  rexmodule-test-dyninit
  rexmodule-test-otheridentity
)
if (WIN32)
  add_dependencies(unit_tests rexhid-xinput)
endif ()

target_compile_options(
  unit_tests PRIVATE -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING -UREXGLUE_ENABLE_PERF_COUNTERS
)

set_target_properties(unit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexkernel>)
rexglue_link_host_allocator(unit_tests)
if (UNIX AND NOT APPLE)
  rexglue_set_rpath(unit_tests "$ORIGIN")
endif ()

add_library(
  profile-macros-enabled OBJECT EXCLUDE_FROM_ALL base/profile_macros_on/profiling_enabled.cpp
)
target_link_libraries(profile-macros-enabled PRIVATE rexbase)
add_test(NAME "unit.raw memory audit lists current sites"
         COMMAND ${CMAKE_COMMAND} -DREXGLUE_ROOT=${PROJECT_SOURCE_DIR} -P
                 ${PROJECT_SOURCE_DIR}/cmake/rexglue_raw_memory_audit.cmake
)
set_tests_properties("unit.raw memory audit lists current sites" PROPERTIES WILL_FAIL TRUE)

add_test(NAME "unit.compile: profiling macros compile with profiling enabled"
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target
                 profile-macros-enabled
)
set_tests_properties(
  "unit.compile: profiling macros compile with profiling enabled" PROPERTIES RESOURCE_LOCK
                                                                             nested_build
)

add_library(stack-frame-push-value OBJECT EXCLUDE_FROM_ALL ppc/stack_frame_fail/push_value.cpp)
target_link_libraries(stack-frame-push-value PRIVATE rexkernel)
add_test(NAME "unit.compile: StackFrame accepts a push of a value"
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target
                 stack-frame-push-value
)
set_tests_properties(
  "unit.compile: StackFrame accepts a push of a value" PROPERTIES RESOURCE_LOCK nested_build
)
foreach (_case IN ITEMS pointer mapped_ptr pointer_array mapped_ptr_array)
  string(REPLACE "_" "-" _target "stack-frame-push-${_case}")
  add_library(${_target} OBJECT EXCLUDE_FROM_ALL ppc/stack_frame_fail/push_${_case}.cpp)
  target_link_libraries(${_target} PRIVATE rexkernel)
  set_target_properties(${_target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
  add_test(NAME "unit.compile: StackFrame rejects a push of a ${_case}"
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target
                   ${_target}
  )
  set_tests_properties(
    "unit.compile: StackFrame rejects a push of a ${_case}"
    PROPERTIES PASS_REGULAR_EXPRESSION "StackFrame cannot push a pointer or a MappedPtr"
               RESOURCE_LOCK nested_build
  )
endforeach ()

add_library(
  midasm-macro-accepts-attachment OBJECT EXCLUDE_FROM_ALL
                                         ppc/midasm_macro_fail/accepts_attachment.cpp
)
target_link_libraries(midasm-macro-accepts-attachment PRIVATE rexkernel)
add_test(NAME "unit.compile: midasm accepts an attachment that fits"
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target
                 midasm-macro-accepts-attachment
)
set_tests_properties(
  "unit.compile: midasm accepts an attachment that fits" PROPERTIES RESOURCE_LOCK nested_build
)
add_executable(midasm-generated-hooks EXCLUDE_FROM_ALL ppc/midasm_generated/generated_hooks.cpp)
rexglue_link_host_allocator(midasm-generated-hooks)
target_include_directories(
  midasm-generated-hooks PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/midasm_generated
)
target_link_libraries(midasm-generated-hooks PRIVATE rexkernel)
set_target_properties(
  midasm-generated-hooks PROPERTIES EXPORT_COMPILE_COMMANDS OFF RUNTIME_OUTPUT_DIRECTORY
                                                                $<TARGET_FILE_DIR:rexkernel>
)
if (UNIX AND NOT APPLE)
  rexglue_set_rpath(midasm-generated-hooks "$ORIGIN")
endif ()
add_test(
  NAME "unit.compile: midasm generated hooks run against their bodies"
  COMMAND
    ${CMAKE_COMMAND} -DUNIT_TESTS=$<TARGET_FILE:unit_tests>
    -DUNIT_TESTS_DIR=$<TARGET_FILE_DIR:unit_tests>
    -DGENERATED_DIR=${CMAKE_CURRENT_BINARY_DIR}/midasm_generated -DBUILD_DIR=${CMAKE_BINARY_DIR}
    -DCONFIG=$<CONFIG> -DHOOKS=$<TARGET_FILE:midasm-generated-hooks> -P
    ${CMAKE_CURRENT_SOURCE_DIR}/ppc/midasm_generated/build_generated_hooks.cmake
)
set_tests_properties(
  "unit.compile: midasm generated hooks run against their bodies" PROPERTIES RESOURCE_LOCK
                                                                             nested_build
)
set(_midasm_fail_cases
    "type_mismatch|register type does not fit its parameter"
    "pointer_float_register|register type does not fit its parameter"
    "unknown_register|register must be r0 to r31 except r1"
    "stack_pointer|register must be r0 to r31 except r1"
    "count_mismatch|register count must equal the body parameters"
    "hook_target|takes a target only for a branch body"
    "branch_without_target|needs the target of a branch body first"
    "bad_target|target must be a nonzero multiple of 4"
    "bad_placement|placement must be before or after"
    "bad_address|address must be a nonzero multiple of 4"
    "bad_parameter|A mid asm body takes u32"
)
foreach (_entry IN LISTS _midasm_fail_cases)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _case)
  list(GET _parts 1 _message)
  string(REPLACE "_" "-" _target "midasm-macro-${_case}")
  add_library(${_target} OBJECT EXCLUDE_FROM_ALL ppc/midasm_macro_fail/${_case}.cpp)
  target_link_libraries(${_target} PRIVATE rexkernel)
  set_target_properties(${_target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
  add_test(NAME "unit.compile: midasm rejects ${_case}"
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target
                   ${_target}
  )
  set_tests_properties(
    "unit.compile: midasm rejects ${_case}" PROPERTIES PASS_REGULAR_EXPRESSION "${_message}"
                                                       RESOURCE_LOCK nested_build
  )
endforeach ()

include(Catch)
set(_unit_test_spec "")
if (REXGLUE_ENABLE_TSAN)
  set(_unit_test_spec TEST_SPEC "~[forced_stop]~[crash_report]")

  # TSan defers asynchronous signals, so the forced stop redirect cannot take effect
  catch_discover_tests(
    unit_tests
    DISCOVERY_MODE
    PRE_TEST
    TEST_SPEC
    "[crash_report]"
    TEST_PREFIX
    "unit."
    ADD_TAGS_AS_LABELS
    PROPERTIES
    TIMEOUT
    60
    ENVIRONMENT
    "TSAN_OPTIONS=handle_segv=0" # TSan takes SIGSEGV for its own report unless handle_segv is 0
  )
elseif (REXGLUE_ENABLE_SANITIZERS)
  set(_unit_test_spec TEST_SPEC "~[crash_report]")

  # UBSan installs its own fault handlers unless they are turned off
  catch_discover_tests(
    unit_tests
    DISCOVERY_MODE
    PRE_TEST
    TEST_SPEC
    "[crash_report]"
    TEST_PREFIX
    "unit."
    ADD_TAGS_AS_LABELS
    PROPERTIES
    TIMEOUT
    60
    ENVIRONMENT
    "UBSAN_OPTIONS=handle_segv=0:handle_sigbus=0:handle_abort=0:handle_sigfpe=0:handle_sigill=0"
  )
endif ()
catch_discover_tests(
  unit_tests
  DISCOVERY_MODE
  PRE_TEST
  ${_unit_test_spec}
  TEST_PREFIX
  "unit."
  ADD_TAGS_AS_LABELS
  PROPERTIES
  TIMEOUT
  60
)
catch_discover_tests(
  cross_module_tests
  DISCOVERY_MODE
  PRE_TEST
  TEST_PREFIX
  "unit."
  ADD_TAGS_AS_LABELS
  PROPERTIES
  TIMEOUT
  60
)
