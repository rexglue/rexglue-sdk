# Unit Tests for ReXGlue core components

add_executable(unit_tests
    test_memory.cpp
    apu/null_backend_test.cpp
    hid/null_backend_test.cpp
    hid/xam_input_error_test.cpp
    hid/input_assignment_test.cpp
    hid/hub_test.cpp
    hid/virtual_key_test.cpp
    memory/heap_allocation_test.cpp
    memory/access_violation_test.cpp
    memory/memory_budget_test.cpp
    gpu/backend_select_test.cpp
    gpu/null_command_processor_test.cpp
    gpu/cvar_scope_test.cpp
    gpu/overlay_contract_test.cpp
    kernel/exit_title_test.cpp
    kernel/video_mode_test.cpp
    kernel/object_table_test.cpp
    kernel/shim_registry_test.cpp
    kernel/exports_test.cpp
    kernel/kernel_fixture.cpp
    kernel/lock_order_test.cpp
    kernel/pinned_wait_test.cpp
    kernel/char_events_test.cpp
    kernel/char_sync_test.cpp
    kernel/char_handles_test.cpp
    kernel/char_threads_test.cpp
    kernel/char_files_test.cpp
    kernel/char_identity_test.cpp
    kernel/interrupt_test.cpp
    kernel/char_dpc_test.cpp
    kernel/char_wait_test.cpp
    kernel/char_notify_test.cpp
    kernel/char_fp_state_test.cpp
    kernel/char_tls_test.cpp
    kernel/char_fiber_test.cpp
    kernel/char_workspace_test.cpp
    core/app_test.cpp
    core/fat_timestamp_test.cpp
    core/cli_arg_list_test.cpp
    core/cvar_test.cpp
    core/config_test.cpp
    core/core_fixes_test.cpp
    core/crash_reporter_test.cpp
    core/fiber_test.cpp
    core/hash_test.cpp
    core/stream_test.cpp
    core/chrono_test.cpp
    core/macros_test.cpp
    core/mmio_test.cpp
    core/module_loader_test.cpp
    core/relaunch_test.cpp
    core/thread_registry_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    runtime/guest_path_test.cpp
    runtime/achievement_manager_test.cpp
    runtime/xcontent_test.cpp
    runtime/function_dispatcher_test.cpp
    runtime/thread_wait_test.cpp
    runtime/runtime_error_test.cpp
    runtime/runtime_interfaces_test.cpp
    runtime/memory_view_test.cpp
    runtime/apu_test.cpp
    runtime/xma_decoder_test.cpp
    runtime/host_service_modules_test.cpp
    runtime/null_gpu_test.cpp
    ui/console_model_test.cpp
    ui/title_app_test.cpp
    ui/ui_event_test.cpp
    codegen/codegen_writer_test.cpp
    codegen/output_partition_test.cpp
    codegen/output_stamp_test.cpp
    codegen/shim_config_test.cpp
    codegen/tool_runtime_test.cpp
    codegen/mmio_analysis_test.cpp
)

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
    ${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c
)

set_source_files_properties(${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c
    PROPERTIES LANGUAGE CXX)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rex-codegen
    rex-runtime
    rex-hid-common
    xxHash::xxhash
    Catch2::Catch2WithMain
    SDL3::Headers
)

add_executable(rex-crash-probe crash_probe.cpp)
target_link_libraries(rex-crash-probe PRIVATE rex-runtime)
set_target_properties(rex-crash-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(rex-crash-probe PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

add_dependencies(unit_tests rex-filesystem rex-gpu-xenos rex-sample-console rex-sample-title rex-crash-probe)

add_dependencies(unit_tests
    rexmod-test-good rexmod-test-missing rexmod-test-badversion
    rexmod-test-failinit rexmod-test-dupecho rexmod-test-null-gpu
    rexmod-test-apu rex-apu-sdl3 rexmod-test-hid rex-hid-sdl3)
if(WIN32)
    add_dependencies(unit_tests rex-hid-xinput)
endif()

# Strip Tracy/profiling instrumentation from test targets
target_compile_options(unit_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rex-runtime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
