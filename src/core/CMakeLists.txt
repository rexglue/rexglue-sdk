add_library(
  rexcore SHARED
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
  hash.cpp
  hid/bindings.cpp
  hid/idle_pad_caps.cpp
  hid/input_source.cpp
  hid/key_capture.cpp
  hid/key_chord.cpp
  hid/null_backend.cpp
  hid/virtual_key.cpp
  recompiled_code.cpp
  log_file.cpp
  log_sinks.cpp
  logger.cpp
  byte_swap.cpp
  mmio.cpp
  module_loader.cpp
  lock_order.cpp
  paths.cpp
  platform/file.cpp
  ring_buffer.cpp
  string_buffer.cpp
  thread_registry.cpp
  threading.cpp
  timer_queue.cpp
  utf8.cpp
  vec128.cpp
)

add_library(rex::core ALIAS rexcore)
set_target_properties(rexcore PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rexcore PRIVATE REX_BUILDING_CORE)
rexglue_add_version_resource(rexcore DESCRIPTION "ReXGlue core")

if (APPLE)
  # Apple's libc++ gates std::jthread and std::stop_token behind the
  # experimental-library flag, and rex::platform::Spawn returns std::jthread.
  target_compile_options(rexcore PUBLIC -fexperimental-library)
endif ()

if (WIN32)
  target_sources(
    rexcore
    PRIVATE clock_win.cpp
            dbg_win.cpp
            dynlib_win.cpp
            crash_win.cpp
            exception_handler_win.cpp
            instance_lock_win.cpp
            fiber_win32.cpp
            log_sinks_win.cpp
            mapped_memory_win.cpp
            memory_win.cpp
            platform/console_win.cpp
            platform/env_win.cpp
            platform/file_win.cpp
            process_resources_win.cpp
            relaunch_win.cpp
            seh_win.cpp
            socket_win.cpp
            system_win.cpp
            threading_win.cpp
            utf8_win.cpp
  )
  target_compile_definitions(rexcore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
elseif (UNIX)
  # Linux and macOS/Darwin share the POSIX sources. Darwin only behavior
  # is selected inside them with #if defined(__APPLE__).
  target_sources(
    rexcore
    PRIVATE clock_posix.cpp
            dbg_posix.cpp
            dynlib_posix.cpp
            crash_posix.cpp
            exception_handler_posix.cpp
            fiber_posix.cpp
            instance_lock_posix.cpp
            log_sinks_posix.cpp
            mapped_memory_posix.cpp
            memory_posix.cpp
            platform/console_posix.cpp
            platform/env_posix.cpp
            platform/file_posix.cpp
            process_resources_posix.cpp
            relaunch_posix.cpp
            seh_posix.cpp
            socket_posix.cpp
            system_posix.cpp
            threading_posix.cpp
            utf8_posix.cpp
  )
endif ()

target_sources(
  rexcore
  PRIVATE xenos/xenos.cpp
          xenos/registers.cpp
          xenos/register_file.cpp
          xenos/ucode.cpp
          xenos/texture_info.cpp
          xenos/texture_info_formats.cpp
          xenos/texture_extent.cpp
          xenos/texture_layout.cpp
          xenos/texture_conversion.cpp
)

target_include_directories(
  rexcore
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(
  rexcore PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty
)

if (APPLE)
  target_link_libraries(rexcore PRIVATE pthread)
elseif (UNIX)
  target_link_libraries(rexcore PRIVATE pthread rt dl)
endif ()

target_link_libraries(
  rexcore
  PUBLIC simde tomlplusplus
  PRIVATE excrypt mspack xxHash::xxhash
)

if (WIN32)
  target_link_libraries(rexcore PRIVATE winmm ws2_32)
else ()
  find_package(Iconv REQUIRED)
  target_link_libraries(rexcore PUBLIC Iconv::Iconv)
endif ()

if (REXGLUE_ENABLE_TRACY)
  target_link_libraries(rexcore PUBLIC $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
  target_compile_definitions(rexcore PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif ()

if (REXGLUE_ENABLE_PERF_COUNTERS)
  target_sources(rexcore PRIVATE perf/counter.cpp)
  target_compile_definitions(
    rexcore PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>
  )
endif ()
