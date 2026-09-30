# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput OBJECT
    input_system.cpp
    device_assignment.cpp
    state_merge.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

rex_target_pch(rexinput
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/platform/thread.h>
    <rex/platform/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
    <rex/ui/window.h>
    <rex/ui/presenter.h>
)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
endif()

target_include_directories(rexinput PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rexinput PUBLIC rexcore rexui SDL3::SDL3)
