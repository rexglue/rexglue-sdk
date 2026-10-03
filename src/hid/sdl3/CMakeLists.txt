add_library(rex-hid-sdl3 SHARED module.cpp sdl3_driver.cpp)
add_library(rex::hid-sdl3 ALIAS rex-hid-sdl3)
set_target_properties(rex-hid-sdl3 PROPERTIES
    EXPORT_NAME hid-sdl3
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rex-hid-sdl3 DESCRIPTION "ReXGlue SDL3 gamepads")
target_include_directories(rex-hid-sdl3 PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rex-hid-sdl3 PRIVATE rex-core rex-hid-common SDL3::SDL3)
