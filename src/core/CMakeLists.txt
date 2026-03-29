# rexcore - Core utilities library
# Foundation types, data structures, and threading primitives

add_library(rexcore STATIC
    arena.cpp
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    clock.cpp
    cvar.cpp
    exception_handler.cpp
    filesystem.cpp
    filesystem_wildcard.cpp
    logging.cpp
    memory.cpp
    mutex.cpp
    ring_buffer.cpp
    sha256.cpp
    string.cpp
    string_buffer.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
)
add_library(rex::core ALIAS rexcore)

# Platform-specific sources
if(WIN32)
    target_sources(rexcore PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        fiber_win32.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        seh_win.cpp
        socket_win.cpp
        string_win.cpp
        threading_win.cpp
    )
elseif(UNIX)
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        fiber_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        seh_posix.cpp
        socket_posix.cpp
        string_posix.cpp
        threading_posix.cpp
    )
endif()

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(UNIX)
    target_link_libraries(rexcore PRIVATE pthread rt dl)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
        spdlog::spdlog
    PRIVATE
        disruptorplus
        utf8cpp
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexcore PUBLIC TracyClient)
    target_compile_definitions(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_sources(rexcore PRIVATE perf/counter.cpp)
    target_compile_definitions(rexcore PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()
