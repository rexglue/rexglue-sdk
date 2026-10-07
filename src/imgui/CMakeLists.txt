set(REXIMGUI_DIR "${REXGLUE_ROOT}/thirdparty/imgui")
set(REXIMGUI_THIRDPARTY_SOURCES
    ${REXIMGUI_DIR}/imgui.cpp
    ${REXIMGUI_DIR}/imgui_draw.cpp
    ${REXIMGUI_DIR}/imgui_tables.cpp
    ${REXIMGUI_DIR}/imgui_widgets.cpp
)

add_library(rex-imgui SHARED
    ${REXIMGUI_THIRDPARTY_SOURCES}
    imgui_dialog.cpp
    style.cpp
)
add_library(rex::imgui ALIAS rex-imgui)
set_target_properties(rex-imgui PROPERTIES
    EXPORT_NAME imgui
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
set_source_files_properties(${REXIMGUI_THIRDPARTY_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
rexglue_add_version_resource(rex-imgui DESCRIPTION "ReXGlue ImGui")

target_compile_definitions(rex-imgui
    PRIVATE REX_BUILDING_IMGUI
    PUBLIC "IMGUI_USER_CONFIG=\"rex/ui/imgui_config.h\"")
target_include_directories(rex-imgui PUBLIC
    $<BUILD_INTERFACE:${REXIMGUI_DIR}>
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rex-imgui PUBLIC rex-core)
