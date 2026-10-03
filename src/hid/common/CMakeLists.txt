add_library(rex-hid-common STATIC
    device_assignment.cpp
    hub.cpp
    idle_pad_driver.cpp
    keyboard_mouse_driver.cpp
    module_base.cpp
    state_merge.cpp
)
add_library(rex::hid-common ALIAS rex-hid-common)
set_target_properties(rex-hid-common PROPERTIES EXPORT_NAME hid-common POSITION_INDEPENDENT_CODE ON)
target_include_directories(rex-hid-common PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rex-hid-common PUBLIC rex-core)
