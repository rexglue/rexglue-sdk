add_library(rex-core SHARED
    api.cpp
    apu/null_backend.cpp
    app.cpp
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    cli_arg_list.cpp
    clock.cpp
    cvar_registry.cpp
    crash.cpp
    exception_handler.cpp
    filesystem/container.cpp
    filesystem/gdfx.cpp
    filesystem/lzx.cpp
    filesystem/stfs.cpp
    filesystem/svod.cpp
    filesystem/xcontent.cpp
    filesystem/xex.cpp
    ${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c
    hash.cpp
    hid/key_chord.cpp
    hid/null_backend.cpp
    hid/virtual_key.cpp
    log_file.cpp
    log_sinks.cpp
    logger.cpp
    memory.cpp
    mmio.cpp
    module_loader.cpp
    lock_order.cpp
    platform/file.cpp
    ring_buffer.cpp
    string_buffer.cpp
    thread_registry.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
)

set_source_files_properties(${PROJECT_SOURCE_DIR}/thirdparty/crypto/rijndael-alg-fst.c
    PROPERTIES LANGUAGE CXX)

add_library(rex::core ALIAS rex-core)
set_target_properties(rex-core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rex-core PRIVATE REX_BUILDING_CORE)
rexglue_add_version_resource(rex-core DESCRIPTION "ReXGlue core")

if(APPLE)
    # Apple's libc++ gates std::jthread and std::stop_token behind the
    # experimental-library flag, and rex::platform::Spawn returns std::jthread.
    target_compile_options(rex-core PUBLIC -fexperimental-library)
endif()

# Platform-specific sources
if(WIN32)
    target_sources(rex-core PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        crash_win.cpp
        exception_handler_win.cpp
        instance_lock_win.cpp
        fiber_win32.cpp
        log_sinks_win.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        platform/console_win.cpp
        platform/env_win.cpp
        platform/file_win.cpp
        relaunch_win.cpp
        seh_win.cpp
        socket_win.cpp
        system_win.cpp
        threading_win.cpp
    )
    target_compile_definitions(rex-core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX)
    # Linux and macOS/Darwin share the POSIX sources; Darwin-specific behavior
    # is selected inside them with #if defined(__APPLE__).
    target_sources(rex-core PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        crash_posix.cpp
        exception_handler_posix.cpp
        fiber_posix.cpp
        instance_lock_posix.cpp
        log_sinks_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        platform/console_posix.cpp
        platform/env_posix.cpp
        platform/file_posix.cpp
        relaunch_posix.cpp
        seh_posix.cpp
        socket_posix.cpp
        system_posix.cpp
        threading_posix.cpp
    )
endif()

target_sources(rex-core PRIVATE
    xenos/xenos.cpp
    xenos/registers.cpp
    xenos/register_file.cpp
    xenos/ucode.cpp
    xenos/texture_info.cpp
    xenos/texture_info_formats.cpp
    xenos/texture_extent.cpp
    xenos/texture_util.cpp
    xenos/texture_conversion.cpp
)

target_include_directories(rex-core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rex-core PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/thirdparty
)

if(APPLE)
    target_link_libraries(rex-core PRIVATE pthread)
elseif(UNIX)
    target_link_libraries(rex-core PRIVATE pthread rt dl)
endif()

target_link_libraries(rex-core
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
    PRIVATE
        mspack
        utf8cpp
        xxHash::xxhash
)

if(WIN32)
    target_link_libraries(rex-core PRIVATE winmm ws2_32)
endif()

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rex-core PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rex-core PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_sources(rex-core PRIVATE perf/counter.cpp)
    target_compile_definitions(rex-core PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()
