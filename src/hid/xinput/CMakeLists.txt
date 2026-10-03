add_library(rex-hid-xinput SHARED module.cpp xinput_driver.cpp)
add_library(rex::hid-xinput ALIAS rex-hid-xinput)
set_target_properties(rex-hid-xinput PROPERTIES
    EXPORT_NAME hid-xinput
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rex-hid-xinput DESCRIPTION "ReXGlue XInput gamepads")
target_include_directories(rex-hid-xinput PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rex-hid-xinput PRIVATE rex-core rex-hid-common)
