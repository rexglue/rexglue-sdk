# rexhid
add_library(
  rexhid-objects OBJECT
  assignment.cpp
  binding.cpp
  idle_pad.cpp
  input_source.cpp
  input_module.cpp
  input_system.cpp
  key_capture.cpp
  key_chord.cpp
  keyboard_mouse_pad.cpp
  keystroke.cpp
  merge.cpp
  virtual_key.cpp
)
add_library(rexhid SHARED module_entry.cpp $<TARGET_OBJECTS:rexhid-objects>)
add_library(rex::hid ALIAS rexhid)
rex_target_headers(rexhid DIRS hid)
rexglue_link_host_allocator(rexhid)
set_target_properties(rexhid PROPERTIES EXPORT_NAME hid)
foreach (hid_target rexhid-objects rexhid)
  set_target_properties(
    ${hid_target}
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(${hid_target} PRIVATE REX_BUILDING_HID)
  target_include_directories(
    ${hid_target} PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
                          ${PROJECT_SOURCE_DIR}/thirdparty
  )
  target_link_libraries(${hid_target} PUBLIC rexbase)
endforeach ()
rex_target_pch(rexhid-objects)
rexglue_add_version_resource(rexhid DESCRIPTION "ReXGlue input")

add_subdirectory(sdl3)
if (WIN32)
  add_subdirectory(xinput)
endif ()
