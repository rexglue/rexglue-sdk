set(REX_UI_SDL3_SOURCES
    app_context.cpp
    app_context_sdl.cpp
    bind_table.cpp
    console/model.cpp
    console/view.cpp
    console/window.cpp
    console_host.cpp
    displays.cpp
    file_dialog.cpp
    hid_host.cpp
    image_decode.cpp
    imgui_drawer.cpp
    message_box.cpp
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/console_commands.cpp
    overlay/console_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/settings_overlay.cpp
    overlay_registry.cpp
    prompts.cpp
    sdl_virtual_key.cpp
    user_interface.cpp
    video_mode.cpp
    window.cpp
    window_sdl.cpp
)
if (WIN32)
  list(APPEND REX_UI_SDL3_SOURCES message_box_win.cpp)
else ()
  list(APPEND REX_UI_SDL3_SOURCES message_box_posix.cpp)
endif ()
if (APPLE)
  list(APPEND REX_UI_SDL3_SOURCES surface_mac.cpp)
elseif (NOT WIN32)
  list(APPEND REX_UI_SDL3_SOURCES surface_gnulinux.cpp)
endif ()

add_library(rexui-sdl3-objects OBJECT module.cpp ${REX_UI_SDL3_SOURCES})
set_target_properties(
  rexui-sdl3-objects PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
)
rex_target_pch(rexui-sdl3-objects <rex/math.h>)
target_include_directories(rexui-sdl3-objects PRIVATE ${REXGLUE_ROOT}/src)
target_include_directories(rexui-sdl3-objects SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/stb)
set(REX_UI_SDL3_LIBS rexcore rexui rexgpu reximgui rexruntime SDL3::SDL3)
if (APPLE)
  list(APPEND REX_UI_SDL3_LIBS "-framework CoreFoundation")
endif ()
target_link_libraries(rexui-sdl3-objects PRIVATE ${REX_UI_SDL3_LIBS})

add_library(rexui-sdl3 SHARED $<TARGET_OBJECTS:rexui-sdl3-objects>)
add_library(rex::ui-sdl3 ALIAS rexui-sdl3)
rexglue_link_host_allocator(rexui-sdl3)
set_target_properties(
  rexui-sdl3
  PROPERTIES EXPORT_NAME ui-sdl3
             LINKER_LANGUAGE CXX
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexui-sdl3 DESCRIPTION "ReXGlue SDL3 user interface")
target_link_libraries(rexui-sdl3 PRIVATE ${REX_UI_SDL3_LIBS})
