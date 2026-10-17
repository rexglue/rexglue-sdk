add_library(rexhid-sdl3 SHARED module.cpp sdl3_driver.cpp)
add_library(rex::hid-sdl3 ALIAS rexhid-sdl3)
set_target_properties(rexhid-sdl3 PROPERTIES
    EXPORT_NAME hid-sdl3
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexhid-sdl3 DESCRIPTION "ReXGlue SDL3 gamepads")
target_include_directories(rexhid-sdl3 PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rexhid-sdl3 PRIVATE rexcore rexhid SDL3::SDL3)
