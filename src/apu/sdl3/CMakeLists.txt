add_library(rex-apu-sdl3 SHARED module.cpp sdl3_backend.cpp)
add_library(rex::apu-sdl3 ALIAS rex-apu-sdl3)
set_target_properties(rex-apu-sdl3 PROPERTIES
    EXPORT_NAME apu-sdl3
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rex-apu-sdl3 DESCRIPTION "ReXGlue SDL3 audio output")
target_include_directories(rex-apu-sdl3 PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rex-apu-sdl3 PRIVATE rex-core SDL3::SDL3)
