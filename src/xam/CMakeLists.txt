set(REXXAM_SOURCES
    xam_state.cpp
    xam_title_lifecycle.cpp
    xam_bodies.cpp
    language.cpp
    achievement_manager.cpp
    app_manager.cpp
    enumerator_object.cpp
    socket_object.cpp
    content_manager.cpp
    content_package.cpp
    user_profile.cpp
    xam_module.cpp
    xam_avatar.cpp
    xam_content.cpp
    xam_content_aggregate.cpp
    xam_content_device.cpp
    xam_debug.cpp
    xam_enum.cpp
    xam_info.cpp
    xam_input.cpp
    xam_locale.cpp
    xam_msg.cpp
    xam_net.cpp
    xam_notify.cpp
    xam_nui.cpp
    xam_party.cpp
    xam_task.cpp
    xam_ui.cpp
    xam_user.cpp
    xam_video.cpp
    xam_voice.cpp
    xam_file.cpp
    xam_memory.cpp
    xam_string.cpp
    app/xam_app.cpp
    app/xgi_app.cpp
    app/xlivebase_app.cpp
    app/xmp_app.cpp
)

list(TRANSFORM REXXAM_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_library(rexxam-objects OBJECT ${REXXAM_SOURCES})
add_library(rexxam SHARED module_entry.cpp $<TARGET_OBJECTS:rexxam-objects>)
add_library(rex::xam ALIAS rexxam)
rex_target_headers(rexxam DIRS xam)
rexglue_link_host_allocator(rexxam)
set_target_properties(rexxam PROPERTIES EXPORT_NAME xam)

foreach (xam_target rexxam-objects rexxam)
  set_target_properties(
    ${xam_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${xam_target} PRIVATE REX_BUILDING_XAM)
  target_include_directories(
    ${xam_target}
    PUBLIC $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_include_directories(${xam_target} PRIVATE ${REXGLUE_ROOT}/src ${REXGLUE_ROOT}/thirdparty)
  target_link_libraries(
    ${xam_target}
    PUBLIC rexbase rexppc simde
    PRIVATE tomlplusplus
  )

  if (REXGLUE_ENABLE_TRACY)
    target_link_libraries(
      ${xam_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>
    )
    target_compile_definitions(
      ${xam_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>
    )
  endif ()

  if (REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(
      ${xam_target} PUBLIC $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>
    )
  endif ()

  if (WIN32)
    target_link_libraries(${xam_target} PRIVATE ws2_32)
    target_compile_definitions(${xam_target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  endif ()
endforeach ()
if (APPLE)
  target_link_options(rexxam PRIVATE -Wl,-undefined,error)
elseif (UNIX)
  target_link_options(rexxam PRIVATE -Wl,--no-undefined)
endif ()
rex_target_pch(
  rexxam-objects <rex/hid/input_system.h> <rex/kernel/title_runtime.h> <rex/kernel/types/content.h>
  <rex/ppc/hook.h>
)
rexglue_add_version_resource(rexxam DESCRIPTION "ReXGlue XAM")
