#Unit Tests for ReXGlue core components

add_executable(
  unit_tests
  test_memory.cpp
  apu/null_backend_test.cpp
  hid/null_backend_test.cpp
  hid/xam_input_error_test.cpp
  hid/input_assignment_test.cpp
  hid/hub_test.cpp
  hid/key_chord_test.cpp
  hid/virtual_key_test.cpp
  memory/heap_allocation_test.cpp
  memory/access_violation_test.cpp
  memory/memory_budget_test.cpp
  memory/byte_swap_test.cpp
  memory/mapped_memory_test.cpp
  memory/file_mapping_test.cpp
  memory/bit_cast_copy_test.cpp
  gpu/backend_select_test.cpp
  gpu/null_command_processor_test.cpp
  gpu/packet_header_test.cpp
  gpu/texture_info_test.cpp
  gpu/texture_layout_test.cpp
  gpu/primitive_processor_simd_test.cpp
  gpu/primitive_processor_test.cpp
  gpu/cvar_scope_test.cpp
  gpu/xenos_module_test.cpp
  gpu/draw_state_test.cpp
  gpu/dxbc_operand_test.cpp
  gpu/overlay_contract_test.cpp
  gpu/shader_interpreter_test.cpp
  gpu/shader_translation_test.cpp
  gpu/tessellation_glsl_test.cpp
  kernel/xekeys_test.cpp
  kernel/bugcheck_test.cpp
  kernel/exit_title_test.cpp
  kernel/interlocked_test.cpp
  kernel/video_mode_test.cpp
  kernel/object_table_test.cpp
  kernel/shim_registry_test.cpp
  kernel/exports_test.cpp
  kernel/exports_visibility_test.cpp
  kernel/kernel_fixture.cpp
  kernel/lock_order_test.cpp
  kernel/pinned_wait_test.cpp
  kernel/events_test.cpp
  kernel/sync_test.cpp
  kernel/enumerate_test.cpp
  kernel/handles_test.cpp
  kernel/format_test.cpp
  kernel/rtl_string_test.cpp
  kernel/strings_test.cpp
  kernel/varargs_test.cpp
  kernel/shutdown_test.cpp
  kernel/threads_test.cpp
  kernel/thread_frame_test.cpp
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
  kernel/notify_listener_test.cpp
  kernel/interrupt_test.cpp
  kernel/dpc_test.cpp
  kernel/wait_test.cpp
  kernel/notify_test.cpp
  kernel/fp_state_test.cpp
  kernel/tls_test.cpp
  kernel/fiber_test.cpp
  kernel/workspace_test.cpp
  kernel/overlapped_test.cpp
  core/assert_test.cpp
  core/container_test.cpp
  core/core_test.cpp
  core/high_resolution_timer_test.cpp
  core/interface_test.cpp
  core/logger_test.cpp
  core/math_test.cpp
  core/propagate_const_test.cpp
  core/sleep_test.cpp
  core/socket_test.cpp
  core/socket_wait_test.cpp
  core/thread_callback_test.cpp
  core/thread_start_data_test.cpp
  core/thread_time_test.cpp
  core/timer_queue_test.cpp
  core/utf8_test.cpp
  core/trim_test.cpp
  core/xex_test.cpp
  filesystem/file_system_test.cpp
  filesystem/host_file_test.cpp
  filesystem/open_node_test.cpp
  filesystem/overwrite_test.cpp
  filesystem/providers_test.cpp
  filesystem/rename_test.cpp
  filesystem/wildcard_test.cpp
  core/app_test.cpp
  core/close_owned_test.cpp
  core/no_throw_test.cpp
  core/fat_timestamp_test.cpp
  core/storage_file_test.cpp
  core/host_file_io_test.cpp
  core/cli_action_test.cpp
  core/cli_arg_list_test.cpp
  core/cvar_test.cpp
  core/proc_maps_test.cpp
  core/endian_atomic_test.cpp
  core/config_test.cpp
  core/crash_reporter_test.cpp
  core/fiber_test.cpp
  core/bit_test.cpp
  core/hash_test.cpp
  core/string_buffer_test.cpp
  core/stream_test.cpp
  core/chrono_test.cpp
  core/color_test.cpp
  core/result_macros_test.cpp
  core/mmio_test.cpp
  core/module_loader_test.cpp
  core/move_only_function_test.cpp
  core/relaunch_test.cpp
  core/posix_wait_test.cpp
  core/thread_redirect_test.cpp
  core/thread_registry_test.cpp
  ppc/dot_product_test.cpp
  ppc/estimate_test.cpp
  ppc/fpscr_flush_test.cpp
  ppc/hook_shapes_test.cpp
  ppc/multiply_add_test.cpp
  ppc/pack_d3d_test.cpp
  ppc/pdata_test.cpp
  ppc/saturation_test.cpp
  ppc/arg_translator_test.cpp
  ppc/stack_test.cpp
  ppc/vector_tables_test.cpp
  runtime/timeout_test.cpp
  runtime/guest_path_test.cpp
  runtime/map_parser_test.cpp
  runtime/guest_allocator_test.cpp
  kernel/shim_heap_test.cpp
  runtime/object_block_test.cpp
  runtime/module_object_test.cpp
  runtime/achievement_manager_test.cpp
  runtime/xcontent_test.cpp
  runtime/xsemaphore_test.cpp
  runtime/user_profile_test.cpp
  runtime/xdbf_test.cpp
  runtime/xtypes_test.cpp
  runtime/function_dispatcher_test.cpp
  runtime/call_test.cpp
  runtime/thread_wait_test.cpp
  runtime/runtime_error_test.cpp
  runtime/runtime_interfaces_test.cpp
  runtime/memory_view_test.cpp
  runtime/apu_test.cpp
  runtime/xma_decoder_test.cpp
  runtime/host_service_modules_test.cpp
  runtime/null_gpu_test.cpp
  runtime/recompiled_module_test.cpp
  runtime/cross_module_import_test.cpp
  ui/bind_test.cpp
  ui/console_model_test.cpp
  ui/imgui_dialog_test.cpp
  ui/null_user_interface_test.cpp
  ui/prompt_test.cpp
  ui/xam_prompt_test.cpp
  ui/style_test.cpp
  ui/imgui_style_test.cpp
  ui/input_listener_list_test.cpp
  ui/title_app_test.cpp
  ui/ui_event_test.cpp
  codegen/barrier_test.cpp
  codegen/codegen_writer_test.cpp
  codegen/disasm_test.cpp
  codegen/flush_mode_test.cpp
  codegen/funclet_sync_test.cpp
  codegen/gap_fill_test.cpp
  codegen/millicode_test.cpp
  codegen/noreturn_test.cpp
  codegen/reserved_register_test.cpp
  codegen/setjmp_test.cpp
  codegen/skip_lr_test.cpp
  codegen/function_scanner_test.cpp
  codegen/manifest_test.cpp
  codegen/midasm_hook_config_test.cpp
  codegen/midasm_hook_emit_test.cpp
  codegen/opcode_test.cpp
  codegen/output_partition_test.cpp
  codegen/output_stamp_test.cpp
  codegen/project_recompiler_test.cpp
  codegen/seh_scope_test.cpp
  codegen/shim_config_test.cpp
  codegen/tool_runtime_test.cpp
  codegen/mmio_analysis_test.cpp
  rexglue/init_command_test.cpp
  rexglue/project_scan_test.cpp
  rexglue/test_spec_test.cpp
)

target_sources(
  unit_tests
  PRIVATE ${PROJECT_SOURCE_DIR}/src/gpu/xenos/pipeline/shader/interpreter.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/pipeline/shader/shader.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/draw_state.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/host/fixed_point.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/index_conversion.cpp
          ${PROJECT_SOURCE_DIR}/src/gpu/xenos/vulkan/tessellation_glsl.cpp
          ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
          ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
          ${PROJECT_SOURCE_DIR}/src/rexglue/commands/init_command.cpp
          ${PROJECT_SOURCE_DIR}/src/rexglue/commands/project_scan.cpp
          ${PROJECT_SOURCE_DIR}/src/rexglue/commands/test_spec.cpp
          ${PROJECT_SOURCE_DIR}/src/filesystem/wildcard.cpp
)

if (WIN32)
  target_sources(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/filesystem/host_file_win.cpp)
elseif (UNIX)
  target_sources(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/filesystem/host_file_posix.cpp)
endif ()

target_include_directories(
  unit_tests
  PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests
          ${PROJECT_SOURCE_DIR}/thirdparty ${PROJECT_SOURCE_DIR}/thirdparty/disasm
          ${CMAKE_CURRENT_SOURCE_DIR}
)
#CLI11 instantiates std::wstring_convert, which libc++ marks deprecated
target_include_directories(unit_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include)

target_link_libraries(
  unit_tests
  PRIVATE rexcodegen
          rexruntime
          excrypt
          reximgui
          rexui
          rexhid
          rexui-sdl3-static
          xxHash::xxhash
          Catch2::Catch2WithMain
          SDL3::Headers
)
if (WIN32)
  target_link_libraries(unit_tests PRIVATE ws2_32)
endif ()

target_include_directories(
  unit_tests SYSTEM
  PRIVATE $<TARGET_PROPERTY:SPIRV,INTERFACE_INCLUDE_DIRECTORIES>
          $<TARGET_PROPERTY:Vulkan::Headers,INTERFACE_INCLUDE_DIRECTORIES>
          $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>
          $<TARGET_PROPERTY:GPUOpen::VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>
          $<TARGET_PROPERTY:spirv-tools-headers,INTERFACE_INCLUDE_DIRECTORIES>
)

target_compile_definitions(
  unit_tests
  PRIVATE REX_TEST_RUNTIME_PATH="$<TARGET_FILE:rexruntime>"
          REX_TEST_RECOMPILED_A_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-recompiled-a>"
          REX_TEST_RECOMPILED_B_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-recompiled-b>"
          REX_TEST_CROSS_EXPORTER_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-cross-exporter>"
          REX_TEST_CROSS_IMPORTER_NAME="$<TARGET_FILE_BASE_NAME:rexmod-test-cross-importer>"
          REXGLUE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/filesystem"
)

add_executable(rex-crash-test crash_child.cpp crash_child_recompiled_code.cpp)
if (WIN32)
  set_source_files_properties(
    crash_child_recompiled_code.cpp PROPERTIES COMPILE_OPTIONS -fasync-exceptions
  )
else ()
  set_source_files_properties(
    crash_child_recompiled_code.cpp PROPERTIES COMPILE_OPTIONS -fasynchronous-unwind-tables
  )
endif ()
target_link_libraries(rex-crash-test PRIVATE rexruntime)
set_target_properties(
  rex-crash-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if (UNIX AND NOT APPLE)
  set_target_properties(
    rex-crash-test PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON
  )
endif ()

add_dependencies(
  unit_tests
  rexfilesystem
  rexgpu-xenos
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
  rexmod-test-good
  rexmod-test-missing
  rexmod-test-badversion
  rexmod-test-failinit
  rexmod-test-duplicate-echo
  rexmod-test-null-gpu
  rexmod-test-noidentity
  rexmod-test-otheridentity
  rexmod-test-apu
  rexapu-sdl3
  rexmod-test-hid
  rexhid-sdl3
  rexui-sdl3
  rexmod-test-recompiled-a
  rexmod-test-recompiled-b
  rexmod-test-cross-exporter
  rexmod-test-cross-importer
)
if (WIN32)
  add_dependencies(unit_tests rexhid-xinput)
endif ()

#Strip Tracy / profiling definitions from test targets
target_compile_options(
  unit_tests PRIVATE -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING -UREXGLUE_ENABLE_PERF_COUNTERS
)

set_target_properties(unit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
if (UNIX AND NOT APPLE)
  set_target_properties(unit_tests PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
endif ()

#Discover tests
include(Catch)
set(_unit_test_spec "")
if (REXGLUE_ENABLE_TSAN)
  set(_unit_test_spec TEST_SPEC "~[forced_stop]~[crash_report]")

  # TSan defers asynchronous signals to the next interceptor, so the forced stop redirect cannot
  # take effect
  catch_discover_tests(
    unit_tests
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
endif ()
catch_discover_tests(
  unit_tests
  ${_unit_test_spec}
  TEST_PREFIX
  "unit."
  ADD_TAGS_AS_LABELS
  PROPERTIES
  TIMEOUT
  60
)
