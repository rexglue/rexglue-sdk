add_library(rexhid-xinput SHARED module.cpp xinput_driver.cpp)
add_library(rex::hid-xinput ALIAS rexhid-xinput)
rexglue_link_host_allocator(rexhid-xinput)
set_target_properties(
  rexhid-xinput
  PROPERTIES EXPORT_NAME hid-xinput
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexhid-xinput DESCRIPTION "ReXGlue XInput gamepads")
target_include_directories(rexhid-xinput PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rexhid-xinput PRIVATE rexcore rexhid)
