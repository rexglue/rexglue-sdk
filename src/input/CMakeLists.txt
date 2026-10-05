# rex-input - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rex-input OBJECT
    input_system.cpp
    device_assignment.cpp
    state_merge.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rex-input)

rex_target_pch(rex-input
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/platform/thread.h>
    <rex/ppc/context.h>
    <rex/runtime.h>
    <rex/runtime/xthread.h>
    <rex/ui/window.h>
    <rex/ui/presenter.h>
)

target_include_directories(rex-input PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rex-input PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
else()
    target_sources(rex-input PRIVATE sdl/sdl_input_driver.cpp)
endif()

target_include_directories(rex-input PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rex-input PUBLIC rex-core rex-ui SDL3::SDL3)
