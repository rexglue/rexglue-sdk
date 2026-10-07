# rex-ui

# Core UI sources
set(REXUI_CORE_SOURCES
    console/model.cpp
    console/view.cpp
    console/window.cpp
    image_decode.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    keybinds.cpp
    overlay.cpp
    sdl_virtual_key.cpp
    title_app.cpp
    video_mode.cpp
    window.cpp
    window_sdl.cpp
    windowed_app_context.cpp
    windowed_app_context_sdl.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/console_commands.cpp
    overlay/settings_overlay.cpp
)

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
)

add_library(rex-ui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rex-ui)
set_target_properties(rex-ui PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rex-ui PRIVATE REX_BUILDING_RUNTIME)

rex_target_pch(rex-ui <rex/math.h>)

target_include_directories(rex-ui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rex-ui PRIVATE
    ${REXGLUE_ROOT}/src
    ${REXGLUE_ROOT}/thirdparty/stb
)

target_link_libraries(rex-ui PUBLIC
    rex-core
    rex-gpu
    imgui::imgui
    SDL3::SDL3
)
target_link_libraries(rex-ui PRIVATE utf8cpp)

if(APPLE)
    target_link_libraries(rex-ui PRIVATE "-framework CoreFoundation")
endif()

