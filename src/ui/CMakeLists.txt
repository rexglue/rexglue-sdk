add_library(
  rexui-objects OBJECT
  imgui_host.cpp
  overlay/achievement_icon_cache.cpp
  overlay/achievement_toast.cpp
  overlay/achievement_overlay.cpp
  overlay/bind_table.cpp
  overlay/console/model.cpp
  overlay/console/view.cpp
  overlay/console_command.cpp
  overlay/console_host.cpp
  overlay/console_overlay.cpp
  overlay/debug_overlay.cpp
  overlay/image_decode.cpp
  overlay/overlay_registry.cpp
  overlay/settings_overlay.cpp
  prompt.cpp
  style.cpp
  ui_event.cpp
  ui_module.cpp
  ui_system.cpp
  video_mode.cpp
  window_settings.cpp
)
add_library(rexui SHARED module_entry.cpp $<TARGET_OBJECTS:rexui-objects>)
add_library(rex::ui ALIAS rexui)
rex_target_headers(rexui DIRS ui)
rexglue_link_host_allocator(rexui)
set_target_properties(rexui PROPERTIES EXPORT_NAME ui)
foreach (ui_target rexui-objects rexui)
  set_target_properties(
    ${ui_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${ui_target} PRIVATE REX_BUILDING_UI)
  target_include_directories(
    ${ui_target} PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
                         ${PROJECT_SOURCE_DIR}/thirdparty
  )
  target_include_directories(${ui_target} SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/stb)
  target_link_libraries(${ui_target} PUBLIC rexbase)
endforeach ()
rex_target_pch(rexui-objects)
rexglue_add_version_resource(rexui DESCRIPTION "ReXGlue UI")

add_subdirectory(sdl3)
