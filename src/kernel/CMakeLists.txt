add_library(rexkernel-objects OBJECT)
add_library(rexkernel SHARED module_entry.cpp $<TARGET_OBJECTS:rexkernel-objects>)
add_library(rex::kernel ALIAS rexkernel)
rex_target_headers(rexkernel DIRS kernel)
rexglue_link_host_allocator(rexkernel)

foreach (kernel_target rexkernel-objects rexkernel)
  set_target_properties(
    ${kernel_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${kernel_target} PRIVATE REX_BUILDING_KERNEL)
endforeach ()
rexglue_add_version_resource(rexkernel DESCRIPTION "ReXGlue runtime")

rex_target_pch(
  rexkernel-objects
  <unordered_map>
  <rex/math.h>
  <rex/string/text.h>
  <rex/memory.h>
  <rex/filesystem/file_system.h>
  <rex/platform/thread.h>
  <rex/ppc/context.h>
  <rex/kernel/title_runtime.h>
)

foreach (kernel_target rexkernel-objects rexkernel)
  target_include_directories(
    ${kernel_target}
    PUBLIC $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_include_directories(
    ${kernel_target} PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty
  )

  target_link_libraries(
    ${kernel_target}
    PUBLIC rexbase rexppc simde
    PRIVATE tomlplusplus excrypt xxHash::xxhash tlsf libavcodec libavutil
  )

  if (REXGLUE_ENABLE_TRACY)
    # TARGET_NAME makes install(EXPORT) namespace the target, a plain name leaves consumers
    # linking a bare TracyClient.
    target_link_libraries(
      ${kernel_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>
    )
    target_compile_definitions(
      ${kernel_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>
    )
  endif ()

  if (REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(
      ${kernel_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>
    )
  endif ()

  if (WIN32)
    target_link_libraries(${kernel_target} PRIVATE ws2_32 winmm)
    target_compile_definitions(${kernel_target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  endif ()
endforeach ()

set(REXKERNEL_SOURCES
    title_runtime.cpp
    rexkernel_module.cpp
    elf_module.cpp
    map_parser.cpp
    apu.cpp
    xma/context.cpp
    xma/decoder.cpp
    xma/register_file.cpp
    xex_module.cpp
    title_context.cpp
    kernel_init.cpp
    export.cpp
    object/event_object.cpp
    object/file_object.cpp
    object/mutant_object.cpp
    object/semaphore_object.cpp
    object/timer_object.cpp
    object/io_completion_object.cpp
    object/notify_listener_object.cpp
    object/symbolic_link_object.cpp
    object/object.cpp
    test_access.cpp
    title_state.cpp
    thread_state.cpp
    kernel_thread.cpp
    thread_registry.cpp
    ppc_hooks.cpp
    guest_stack.cpp
    process.cpp
    dispatcher.cpp
    notification_broadcaster.cpp
    object_table.cpp
    overlapped.cpp
    module.cpp
    module_import.cpp
    module_object.cpp
    module_registry.cpp
    kernel_module.cpp
    user_module.cpp
    memory.cpp
    guest_memory.cpp
    best_fit_range_allocator.cpp
    guest_allocator.cpp
    workspace_heap.cpp
    function_dispatcher.cpp
    # __imp__Xbdm* symbols
    xbdm/forward.cpp
    xbdm/xbdm_forwards.cpp
    xbdm/xbdm_module.cpp
    # __imp__ symbols for generated code
    xboxkrnl/xboxkrnl_crypt.cpp
    xboxkrnl/xboxkrnl_debug.cpp
    xboxkrnl/xboxkrnl_error.cpp
    xboxkrnl/xboxkrnl_hal.cpp
    xboxkrnl/xboxkrnl_hid.cpp
    xboxkrnl/xboxkrnl_io.cpp
    xboxkrnl/xboxkrnl_io_info.cpp
    xboxkrnl/xboxkrnl_memory.cpp
    xboxkrnl/xboxkrnl_stub.cpp
    xboxkrnl/xboxkrnl_module.cpp
    xboxkrnl/xboxkrnl_xex.cpp
    xboxkrnl/xboxkrnl_ob.cpp
    xboxkrnl/xboxkrnl_rtl.cpp
    xboxkrnl/xboxkrnl_strings.cpp
    xboxkrnl/xboxkrnl_threading.cpp
    xboxkrnl/xboxkrnl_usbcam.cpp
    xboxkrnl/xboxkrnl_video.cpp
    xboxkrnl/xboxkrnl_xconfig.cpp
    xboxkrnl/loaded_command_line.cpp
    xboxkrnl/xboxkrnl_audio.cpp
    xboxkrnl/xboxkrnl_audio_xma.cpp
    shim/heap.cpp
    shim/debug.cpp
    shim/file.cpp
    shim/math.cpp
    shim/memory.cpp
    shim/string.cpp
    shim/threading.cpp
    xam/export.cpp
    xam/forward.cpp
    xam/xam_forwards.cpp
    xam/xam_stub.cpp
)

list(TRANSFORM REXKERNEL_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(rexkernel-objects PRIVATE ${REXKERNEL_SOURCES})
