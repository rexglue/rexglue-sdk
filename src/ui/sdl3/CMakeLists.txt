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
if(WIN32)
    list(APPEND REX_UI_SDL3_SOURCES message_box_win.cpp)
else()
    list(APPEND REX_UI_SDL3_SOURCES message_box_posix.cpp)
endif()
if(APPLE)
    list(APPEND REX_UI_SDL3_SOURCES surface_mac.cpp)
elseif(NOT WIN32)
    list(APPEND REX_UI_SDL3_SOURCES surface_gnulinux.cpp)
endif()

function(rex_ui_sdl3_configure target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    rex_target_pch(${target_name} <rex/math.h>)
    target_include_directories(${target_name} PRIVATE ${REXGLUE_ROOT}/src)
    target_include_directories(${target_name} SYSTEM PRIVATE ${REXGLUE_ROOT}/thirdparty/stb)
    target_link_libraries(${target_name} PRIVATE
        rex-core rex-ui rex-gpu rex-imgui rex-runtime SDL3::SDL3)
    if(APPLE)
        target_link_libraries(${target_name} PRIVATE "-framework CoreFoundation")
    endif()
endfunction()

add_library(rex-ui-sdl3 SHARED module.cpp ${REX_UI_SDL3_SOURCES})
add_library(rex::ui-sdl3 ALIAS rex-ui-sdl3)
set_target_properties(rex-ui-sdl3 PROPERTIES EXPORT_NAME ui-sdl3)
rexglue_add_version_resource(rex-ui-sdl3 DESCRIPTION "ReXGlue SDL3 user interface")
rex_ui_sdl3_configure(rex-ui-sdl3)

if(REXGLUE_BUILD_TESTS)
    add_library(rex-ui-sdl3-static STATIC module.cpp ${REX_UI_SDL3_SOURCES})
    set_target_properties(rex-ui-sdl3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    rex_ui_sdl3_configure(rex-ui-sdl3-static)
endif()
