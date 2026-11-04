add_library(rexbase SHARED)

# Each group below names the target it moves to when the library splits.

# rexbase
target_sources(
  rexbase
  PRIVATE bit_map.cpp
          bit_stream.cpp
          byte_stream.cpp
          byte_swap.cpp
          cli_arg_list.cpp
          clock.cpp
          crash.cpp
          exception_handler.cpp
          hash.cpp
          init.cpp
          host_alloc.cpp
          library_name.cpp
          lock_order.cpp
          log_file.cpp
          log_sink.cpp
          logger.cpp
          module_group.cpp
          path.cpp
          paths.cpp
          process_control.cpp
          driver_context.cpp
          driver_select.cpp
          platform/file.cpp
          process_lifecycle.cpp
          ring_buffer.cpp
          settings/aliases.cpp
          settings/parse.cpp
          settings/setting.cpp
          settings/settings.cpp
          settings/toml.cpp
          status.cpp
          string_buffer.cpp
          host_thread_registry.cpp
          threading.cpp
          timer_queue.cpp
          utf8.cpp
          utility.cpp
          ${PROJECT_BINARY_DIR}/generated/base/build_stamp.cpp
)

# rexapp

add_library(rex::base ALIAS rexbase)
rex_target_headers(
  rexbase
  TOP
  DIRS
  logging
  memory
  platform
  string
  GENERATED
  rex/version.h
)
rexglue_link_host_allocator(rexbase)
set_target_properties(rexbase PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rexbase PRIVATE REX_BUILDING_BASE)
rex_target_pch(rexbase)

# Installed modules sit in the library folder, so the loader also looks there from the executable.
if (UNIX)
  target_compile_definitions(
    rexbase PRIVATE REX_INSTALL_LIBDIR_FROM_BINDIR="${REXGLUE_LIBDIR_FROM_BINDIR}"
  )
else ()
  target_compile_definitions(rexbase PRIVATE REX_INSTALL_LIBDIR_FROM_BINDIR="")
endif ()
rexglue_add_version_resource(rexbase DESCRIPTION "ReXGlue base")

if (APPLE)
  # Apple's libc++ hides std::jthread behind this flag, and rex::platform::Spawn returns it.
  target_compile_options(rexbase PUBLIC -fexperimental-library)
endif ()

# rexbase and, for the seh files, rexppc
if (WIN32)
  target_sources(
    rexbase
    PRIVATE clock_win.cpp
            dbg_win.cpp
            dynlib_win.cpp
            crash_win.cpp
            exception_handler_win.cpp
            instance_lock_win.cpp
            fiber_win32.cpp
            log_sink_win.cpp
            mapped_memory_win.cpp
            memory_win.cpp
            platform/console_win.cpp
            platform/env_win.cpp
            platform/file_win.cpp
            process_resources_win.cpp
            relaunch_win.cpp
            socket_win.cpp
            system_win.cpp
            threading_win.cpp
            utf8_win.cpp
  )
  target_compile_definitions(rexbase PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
elseif (UNIX)
  # Linux and macOS share the POSIX sources.
  target_sources(
    rexbase
    PRIVATE clock_posix.cpp
            dbg_posix.cpp
            dynlib_posix.cpp
            crash_posix.cpp
            exception_handler_posix.cpp
            fiber_posix.cpp
            instance_lock_posix.cpp
            log_sink_posix.cpp
            mapped_memory_posix.cpp
            memory_posix.cpp
            platform/console_posix.cpp
            platform/env_posix.cpp
            platform/file_posix.cpp
            process_resources_posix.cpp
            relaunch_posix.cpp
            socket_posix.cpp
            system_posix.cpp
            threading_posix.cpp
            utf8_posix.cpp
  )
  # These define _XOPEN_SOURCE before any system header, which a precompiled header would precede.
  set_source_files_properties(
    exception_handler_posix.cpp fiber_posix.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON
  )
endif ()

target_include_directories(
  rexbase
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# rexbase
set(REXGLUE_IMGUI_DIR "${REXGLUE_ROOT}/thirdparty/imgui")
set(REXGLUE_IMGUI_SOURCES
    ${REXGLUE_IMGUI_DIR}/imgui.cpp ${REXGLUE_IMGUI_DIR}/imgui_draw.cpp
    ${REXGLUE_IMGUI_DIR}/imgui_tables.cpp ${REXGLUE_IMGUI_DIR}/imgui_widgets.cpp
)
target_sources(rexbase PRIVATE ${REXGLUE_IMGUI_SOURCES})
set_source_files_properties(${REXGLUE_IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
target_compile_definitions(rexbase PUBLIC "IMGUI_USER_CONFIG=\"rex/ui/imgui_config.h\"")
target_include_directories(rexbase PUBLIC $<BUILD_INTERFACE:${REXGLUE_IMGUI_DIR}>)
target_include_directories(rexbase SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/stb)

target_include_directories(
  rexbase PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)

if (APPLE)
  target_link_libraries(rexbase PRIVATE pthread)
elseif (UNIX)
  target_link_libraries(rexbase PRIVATE pthread rt dl)
endif ()

target_link_libraries(
  rexbase
  PUBLIC simde
  PRIVATE tomlplusplus xxHash::xxhash
)

if (WIN32)
  target_link_libraries(rexbase PRIVATE winmm ws2_32)
else ()
  find_package(Iconv REQUIRED)
  target_link_libraries(rexbase PUBLIC Iconv::Iconv)
endif ()

if (REXGLUE_ENABLE_TRACY)
  target_link_libraries(rexbase PUBLIC $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
  target_compile_definitions(rexbase PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif ()

if (REXGLUE_ENABLE_PERF_COUNTERS)
  target_sources(rexbase PRIVATE perf/counter.cpp)
  target_compile_definitions(
    rexbase PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>
  )
endif ()
