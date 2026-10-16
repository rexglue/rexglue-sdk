add_library(rexhid-common STATIC
    device_assignment.cpp
    hub.cpp
    idle_pad_driver.cpp
    keyboard_mouse_driver.cpp
    module_base.cpp
    state_merge.cpp
)
add_library(rex::hid-common ALIAS rexhid-common)
set_target_properties(rexhid-common PROPERTIES EXPORT_NAME hid-common POSITION_INDEPENDENT_CODE ON)
target_include_directories(rexhid-common PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(rexhid-common PUBLIC rexcore)

add_subdirectory(sdl3)
if(WIN32)
    add_subdirectory(xinput)
endif()
