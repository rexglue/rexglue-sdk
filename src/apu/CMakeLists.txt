add_library(rexapu-objects OBJECT audio_module.cpp audio_system.cpp mix.cpp)
add_library(rexapu SHARED module_entry.cpp $<TARGET_OBJECTS:rexapu-objects>)
add_library(rex::apu ALIAS rexapu)
rexglue_link_host_allocator(rexapu)
set_target_properties(rexapu PROPERTIES EXPORT_NAME apu)
foreach (apu_target rexapu-objects rexapu)
  set_target_properties(
    ${apu_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${apu_target} PRIVATE REX_BUILDING_APU)
  target_include_directories(
    ${apu_target} PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
                          ${PROJECT_SOURCE_DIR}/thirdparty
  )
  target_link_libraries(${apu_target} PRIVATE rexbase)
endforeach ()
rexglue_add_version_resource(rexapu DESCRIPTION "ReXGlue audio")

add_subdirectory(sdl3)
