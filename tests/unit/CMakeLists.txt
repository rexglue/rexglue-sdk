# Unit Tests for ReXGlue core components

add_executable(unit_tests
    audio/output_state_test.cpp
    ui/bar_fill_test.cpp
    ui/detached_presentation_test.cpp
    ui/frame_rate_meter_test.cpp
    ui/notifications_test.cpp
    ui/settings_rows_test.cpp
    ui/window_size_test.cpp
    graphics/draw_capture_mailbox_test.cpp
    graphics/command_processor_swap_test.cpp
    graphics/swap_presentation_test.cpp
    graphics/prepared_resource_input_test.cpp
    graphics/frame_input_costs_test.cpp
    graphics/frame_input_buffer_versions_test.cpp
    graphics/frame_input_native_sources_test.cpp
    graphics/shared_memory_test.cpp
    graphics/texture_memory_version_test.cpp
    graphics/draw_resolution_scale_test.cpp
    test_memory.cpp
    memory/heap_allocation_test.cpp
    memory/physical_write_watch_test.cpp
    kernel/object_table_test.cpp
    kernel/video_delegate_test.cpp
    kernel/video_mode_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/filesystem_test.cpp
    filesystem/virtual_file_system_test.cpp
    core/first_seen_test.cpp
    core/hash_test.cpp
    core/sha256_test.cpp
    core/relay_test.cpp
    core/relay_directory_test.cpp
    core/relay_direct_test.cpp
    core/relay_invite_test.cpp
    core/live_invite_test.cpp
    discord/discord_rpc_test.cpp
    core/stream_test.cpp
    core/system_test.cpp
    core/chrono_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    system/guest_path_test.cpp
    system/profile_test.cpp
    system/achievement_manager_test.cpp
    system/function_dispatcher_test.cpp
    system/mod_catalog_test.cpp
    system/mod_script_catalog_test.cpp
    system/asset_overlay_test.cpp
    system/mod_loadout_test.cpp
    kernel/xam/host_session_data_test.cpp
    kernel/xam/lan_system_link_test.cpp
    kernel/xam/live_sessions_test.cpp
    kernel/xam/live_invites_test.cpp
    kernel/xam/relay_system_link_test.cpp
    kernel/xam/relay_direct_link_test.cpp
    kernel/xam/system_link_config_test.cpp
    kernel/xam/xgi_request_test.cpp
    kernel/xam/xlivebase_request_test.cpp
    graphics/diagnostic_util_test.cpp
    graphics/diagnostic_draw_capture_test.cpp
    graphics/diagnostic_frame_capture_test.cpp
    graphics/xenos_fence_trace_test.cpp
    input/input_assignment_test.cpp
    codegen/codegen_writer_test.cpp
    codegen/output_partition_test.cpp
    codegen/output_stamp_test.cpp
)

if(WIN32 AND REXGLUE_USE_D3D12)
    target_sources(unit_tests PRIVATE
        ui/d3d12_presenter_test.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_alu.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_memexport.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator_om.cpp
        ${PROJECT_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
    )
    target_compile_definitions(unit_tests PRIVATE REX_TEST_DXBC_TRANSLATION=1)
    target_link_libraries(unit_tests PRIVATE d3d12)
endif()

if(REXGLUE_USE_VULKAN)
    target_sources(unit_tests PRIVATE
        graphics/spirv_translator_fetch_test.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_builder.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/spirv_translator_rb.cpp
    )
    target_link_libraries(unit_tests PRIVATE Vulkan::Headers glslang::SPIRV glslang)
endif()

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/graphics_system.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/sampler_info.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/diagnostic_util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/shared_memory.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/cache.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/format/ucode.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/shader.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/util/draw.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/graphics_util.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/d3d12/frame_capture.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics/d3d12/frame_input_draw_builder.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    rexrelay_server
    Catch2::Catch2WithMain
)
if(WIN32)
    target_link_libraries(unit_tests PRIVATE ws2_32)
endif()

if(WIN32 AND REXGLUE_USE_D3D12)
    # Private bridge archive for the title's isolated prepared-input proof.
    # It keeps the SDK's analyzed shader, texture and guard objects behind the
    # test boundary without exporting them through the runtime package.
    add_library(prepared_frame_input_test_support STATIC
        graphics/prepared_frame_input_test_support.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/d3d12/frame_input_draw_builder.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/d3d12/frame_input_sampler.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/graphics_system.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/sampler_info.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/diagnostic_util.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/flags.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/shared_memory.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/texture/cache.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/format/ucode.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/shader.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/util/draw.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/graphics_util.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/d3d12/frame_capture.cpp
        ${PROJECT_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc.cpp
    )
    target_compile_definitions(prepared_frame_input_test_support PRIVATE REX_TEST_DXBC_TRANSLATION=1)
    target_include_directories(prepared_frame_input_test_support PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(prepared_frame_input_test_support PRIVATE
        rexcodegen
        rexruntime
        d3d12
    )
    if(WIN32)
        target_link_libraries(prepared_frame_input_test_support PRIVATE ws2_32)
    endif()
    target_link_libraries(unit_tests PRIVATE prepared_frame_input_test_support)
endif()

add_executable(mod_plugin_tests
    system/mod_plugin_test.cpp
)
target_include_directories(mod_plugin_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(mod_plugin_tests PRIVATE
    rexruntime
    Catch2::Catch2WithMain
)

set(REX_TEST_MODS_ROOT "${CMAKE_CURRENT_BINARY_DIR}/mods")
if(REX_PLATFORM STREQUAL "win-amd64")
    set(REX_TEST_MOD_PLATFORM "windows-x64")
elseif(REX_PLATFORM STREQUAL "win-arm64")
    set(REX_TEST_MOD_PLATFORM "windows-arm64")
elseif(REX_PLATFORM STREQUAL "linux-amd64")
    set(REX_TEST_MOD_PLATFORM "linux-x64")
elseif(REX_PLATFORM STREQUAL "linux-arm64")
    set(REX_TEST_MOD_PLATFORM "linux-arm64")
elseif(REX_PLATFORM STREQUAL "mac-amd64")
    set(REX_TEST_MOD_PLATFORM "macos-x64")
elseif(REX_PLATFORM STREQUAL "mac-arm64")
    set(REX_TEST_MOD_PLATFORM "macos-arm64")
else()
    message(FATAL_ERROR "No mod package platform mapping for ${REX_PLATFORM}")
endif()
add_library(test_mod_plugin SHARED system/test_mod_plugin.cpp)
add_library(test_mod_bad_abi SHARED system/test_mod_bad_abi.cpp)
foreach(target test_mod_plugin test_mod_bad_abi)
    target_link_libraries(${target} PRIVATE rexruntime)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${REX_TEST_MODS_ROOT}/$<TARGET_PROPERTY:${target},OUTPUT_NAME>/code/${REX_TEST_MOD_PLATFORM}"
        RUNTIME_OUTPUT_DIRECTORY "${REX_TEST_MODS_ROOT}/$<TARGET_PROPERTY:${target},OUTPUT_NAME>/code/${REX_TEST_MOD_PLATFORM}"
        DEBUG_POSTFIX "d"
        RELWITHDEBINFO_POSTFIX "rd"
        RELEASE_POSTFIX ""
    )
endforeach()
set_target_properties(test_mod_plugin PROPERTIES OUTPUT_NAME lifecycle)
set_target_properties(test_mod_bad_abi PROPERTIES OUTPUT_NAME bad_abi)
target_compile_definitions(mod_plugin_tests PRIVATE
    REX_TEST_MOD_PLUGIN_PATH="$<TARGET_FILE:test_mod_plugin>"
    REX_TEST_BAD_MOD_PLUGIN_PATH="$<TARGET_FILE:test_mod_bad_abi>"
)
add_dependencies(mod_plugin_tests test_mod_plugin test_mod_bad_abi)

# ReXApp is shipped as source for recomp projects, rather than compiled into
# rexui. Keep its SDK-consumer compilation path covered by the test build.
add_library(rexapp_compile_test OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/rex_app.cpp
)
target_link_libraries(rexapp_compile_test PRIVATE
    rexruntime
    rexui
    rexinput
    rexaudio
)
add_dependencies(unit_tests rexapp_compile_test)
target_sources(unit_tests PRIVATE $<TARGET_OBJECTS:rexapp_compile_test>)
target_include_directories(unit_tests PRIVATE
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
)

# Strip Tracy/profiling instrumentation from test targets
foreach(target unit_tests mod_plugin_tests rexapp_compile_test)
    target_compile_options(${target} PRIVATE
        -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
        -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
    )
endforeach()

set_target_properties(unit_tests mod_plugin_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests mod_plugin_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# The full suite includes host-specific cases. Unix artifact builds exercise
# the portable mod loader executable.
if(NOT REX_PLATFORM MATCHES "^win-")
    set_target_properties(unit_tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    ADD_TAGS_AS_LABELS
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
# [.network] cases bind non-loopback or wildcard addresses, which raises a
# firewall prompt; Catch2 hides them from default runs. This discovery matches
# only hidden cases, since visible [network] cases are registered above.
if(REXGLUE_NETWORK_TESTS)
    catch_discover_tests(unit_tests
        TEST_SPEC "[.][network]"
        TEST_PREFIX "unit."
        TEST_LIST unit_network_tests
        ADD_TAGS_AS_LABELS
        PROPERTIES
            LABELS "unit"
            TIMEOUT 60
    )
endif()
catch_discover_tests(mod_plugin_tests
    TEST_PREFIX "mod_plugin."
    PROPERTIES
        LABELS "mod_plugin"
        TIMEOUT 60
)

# Compile the allocator-injection seam only into this isolated test executable.
add_executable(mod_script_tests system/mod_script_test.cpp
    ${PROJECT_SOURCE_DIR}/src/system/mod_script.cpp)
target_compile_definitions(mod_script_tests PRIVATE REX_MOD_SCRIPT_TESTING=1)
target_include_directories(mod_script_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(mod_script_tests PRIVATE rex_lua Catch2::Catch2WithMain)
catch_discover_tests(mod_script_tests TEST_PREFIX "mod_script."
    PROPERTIES LABELS "mod_script" TIMEOUT 60)

add_executable(mod_script_activation_tests system/mod_script_activation_test.cpp
    ${PROJECT_SOURCE_DIR}/src/system/mod_script_dispatch.cpp)
target_link_libraries(mod_script_activation_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(mod_script_activation_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
catch_discover_tests(mod_script_activation_tests TEST_PREFIX "mod_script."
    PROPERTIES LABELS "mod_script" TIMEOUT 60)

# Initializes its own guest Memory, so it stays out of unit_tests, which shares one.
# Windows only, like unit_tests; defining it elsewhere would register a NOT_BUILT test.
if(REX_PLATFORM MATCHES "^win-")
    add_executable(guest_memory_section_tests memory/guest_memory_section_test.cpp)
    target_include_directories(guest_memory_section_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(guest_memory_section_tests PRIVATE rexruntime Catch2::Catch2WithMain)
    target_compile_options(guest_memory_section_tests PRIVATE
        -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
        -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
    )
    set_target_properties(guest_memory_section_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    )
    catch_discover_tests(guest_memory_section_tests TEST_PREFIX "guest_memory_section."
        PROPERTIES LABELS "guest_memory_section" TIMEOUT 60)
endif()
