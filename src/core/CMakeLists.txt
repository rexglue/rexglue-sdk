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
        debugging_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        mapped_memory_win.cpp
        memory_win.cpp
    )
elseif(UNIX)
    target_sources(rexcore PRIVATE
        debugging_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        mapped_memory_posix.cpp
        memory_posix.cpp
    )
endif()

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(UNIX)
    target_link_libraries(rexcore PRIVATE pthread rt)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        date::date
        simde
        tomlplusplus
        spdlog::spdlog
    PRIVATE
        disruptorplus
        utf8cpp
)
