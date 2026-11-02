add_library(rexapu-sdl3 SHARED sdl3_audio_driver.cpp)
add_library(rex::apu-sdl3 ALIAS rexapu-sdl3)
rexglue_link_host_allocator(rexapu-sdl3)
set_target_properties(
  rexapu-sdl3
  PROPERTIES EXPORT_NAME apu-sdl3
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexapu-sdl3 DESCRIPTION "ReXGlue SDL3 audio output")
target_include_directories(rexapu-sdl3 PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rexapu-sdl3 PRIVATE rexbase SDL3::SDL3)
