set(REX_UI_SDL3_SOURCES displays.cpp file_dialog.cpp message_box.cpp sdl3_ui_driver.cpp
                        sdl_virtual_key.cpp
)
if (WIN32)
  list(APPEND REX_UI_SDL3_SOURCES message_box_win.cpp)
else ()
  list(APPEND REX_UI_SDL3_SOURCES message_box_posix.cpp)
endif ()

add_library(rexui-sdl3 SHARED ${REX_UI_SDL3_SOURCES})
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
target_include_directories(rexui-sdl3 PRIVATE ${REXGLUE_ROOT}/src)
set(REX_UI_SDL3_LIBS rexbase SDL3::SDL3)
if (APPLE)
  list(APPEND REX_UI_SDL3_LIBS "-framework CoreFoundation")
endif ()
target_link_libraries(rexui-sdl3 PRIVATE ${REX_UI_SDL3_LIBS})
